// Purpose: commit authenticated callback-free execution gaps and exact event repeats by reserving
// their journal and audit records before changing any canonical business state.

#include "../recovery/private_event_journal_reservation.hpp"
#include "private_order_reconciler.hpp"
#include "submission_coordinator.hpp"

#include <array>
#include <utility>
#include <variant>

namespace aegis::runtime {
namespace {

// --------------------------------------------------------
// Map a checked resource or state failure to its bounded account-containment cause.
[[nodiscard]] risk::AccountSafetyReason
account_reason_from_gap_commit_error(model::DomainErrorCode code) noexcept {
  using Code = model::DomainErrorCode;
  switch (code) {
  case Code::JournalCapacityExceeded:
  case Code::PrivateEvidenceExhausted:
  case Code::RecoveryCounterExhausted:
    return risk::AccountSafetyReason::EvidenceCapacityExhausted;
  case Code::RecoveryProvenanceMismatch:
    return risk::AccountSafetyReason::ProvenanceMismatch;
  default:
    return risk::AccountSafetyReason::ArithmeticOrStateCapacityFailure;
  }
}

// --------------------------------------------------------
// A tentative claim conflicts when exactly one end of the one-to-one association is shared.
[[nodiscard]] bool
has_conflicting_mapping_claim(const PreparedPrivateMappingCandidate& prior,
                              const PrivateExchangeOrderMapping& candidate) noexcept {
  return (prior.exchange_order_key == candidate.exchange_order_key) !=
         (prior.order_id == candidate.order_id);
}

// --------------------------------------------------------
// Scan one release-published immutable prefix. Admission attempts are globally ordered, and a
// reconciliation row cannot satisfy the ordinary oracle or vice versa.
[[nodiscard]] std::optional<oms::PrivateEventDisposition>
find_committed_gap_disposition(const PrivateBusinessEvidenceStore& evidence,
                               model::AdmissionOrdinal attempt_ordinal,
                               bool reconciliation) noexcept {
  const auto count = evidence.accepted_private_event_record_count();
  for (std::uint32_t index = 0U; index < count; ++index) {
    const auto* record = evidence.accepted_private_event_record_at(index);
    if (record->admission_ordinal() == attempt_ordinal &&
        (record->origin() == oms::PrivateEventOrigin::Reconciliation) == reconciliation) {
      return record->disposition();
    }
    if (record->admission_ordinal() >= attempt_ordinal) {
      break;
    }
  }
  return std::nullopt;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Consult only the ordinary lane of the immutable committed evidence prefix.
std::optional<oms::PrivateEventDisposition>
PrivateOrderReconciler::find_committed_private_event_disposition(
    model::AdmissionOrdinal attempt_ordinal) const noexcept {
  return find_committed_gap_disposition(*business_evidence_store_, attempt_ordinal, false);
}

// --------------------------------------------------------
// Consult only the reconciliation lane of the immutable committed evidence prefix.
std::optional<oms::PrivateEventDisposition>
PrivateOrderReconciler::find_committed_reconciliation_event_disposition(
    model::AdmissionOrdinal attempt_ordinal) const noexcept {
  return find_committed_gap_disposition(*business_evidence_store_, attempt_ordinal, true);
}

// --------------------------------------------------------
// Keep receipt authority inside the guarded owner turn. Compare the event before correlation,
// preflight the entire callback-free transaction, and publish the completion oracle last.
PrivateOrderReconciler::CallbackFreePrivateCommitResult
PrivateOrderReconciler::commit_callback_free_private_input_if_supported(
    const oms::NormalizedPrivateOrderInput& input, const AdmissionReceipt& receipt) {
  using Disposition = oms::PrivateEventDisposition;
  using Code = model::DomainErrorCode;
  using Reason = risk::AccountSafetyReason;
  if (input.origin() != oms::PrivateEventOrigin::Venue &&
      input.origin() != oms::PrivateEventOrigin::Reconciliation) {
    return {};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Compare the complete canonical event before resolving locators or looking up trade identity.
  // A repeat of a merely retained input must not become a new canonical input in this slice.
  const auto semantic = oms::PrivateEventIngressSemanticValue::from_normalized_input(input);
  const auto event_key = oms::PrivateEventRegistryKey::from_ingress_semantic_value(semantic);
  const auto* existing = find_committed_event_identity(event_key);
  if (existing != nullptr && existing->ingress_semantic_value != semantic) {
    return {std::nullopt,
            model::DomainError::create_at_field(Code::PrivateEventConflict,
                                                "private_gap_commit.event_conflict"),
            Reason::EventIdentityConflict, existing->resolution};
  }
  if (existing == nullptr && retention_.preparations.find_prepared_event(event_key) != nullptr) {
    return {};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A failed preflight can retain a receipt without inserting a comparison row. Keep those
  // source facts authoritative for non-promotion, even after the resource becomes available.
  const auto retained_count = retention_.published_turn_count.load(std::memory_order_relaxed);
  if (existing == nullptr) {
    for (std::size_t index = 0U; index < retained_count; ++index) {
      const auto& record = retention_.turns[index];
      const auto retained =
          oms::PrivateEventIngressSemanticValue::from_normalized_input(*record.normalized);
      if (oms::PrivateEventRegistryKey::from_ingress_semantic_value(retained) == event_key) {
        const bool exact = retained == semantic;
        return {std::nullopt,
                model::DomainError::create_at_field(
                    exact ? Code::ReconciliationIncomplete : Code::PrivateEventConflict,
                    exact ? "private_gap_commit.retained_event_repeat"
                          : "private_gap_commit.retained_event_conflict"),
                exact ? record.safety_reason.value_or(Reason::IncompleteReconciliation)
                      : Reason::EventIdentityConflict,
                record.first_admission_resolution};
      }
    }
  }
  const auto* reconciliation =
      std::get_if<oms::ReconciliationPrivateEventOrigin>(&input.origin_value());
  if (reconciliation != nullptr &&
      reconciliation->reconciliation_epoch_id.runtime_epoch_id() != runtime_epoch_id_) {
    return {std::nullopt,
            model::DomainError::create_at_field(Code::RecoveryProvenanceMismatch,
                                                "private_gap_commit.reconciliation_epoch"),
            Reason::ProvenanceMismatch,
            oms::PrivateEventResolution::create_provenance_conflict_resolution()};
  }
  const auto* execution = std::get_if<oms::ExecutionPayload>(&input.payload());
  if (execution == nullptr) {
    return {};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reuse the original event resolution for duplicates. First-seen executions must correlate and
  // own a new trade before the planner can consider changing the pending interval set.
  std::optional<FirstSeenAuthoritativePrivateIdentityPlan> identity;
  std::optional<oms::PrivateEventResolution> resolution;
  std::optional<oms::PrivateTradeSemanticValue> trade;
  RetainedPrivateExecutionProgress progress;
  if (existing != nullptr) {
    resolution = existing->resolution;
    const auto* retained_trade = find_committed_trade_identity(
        oms::TradeKey{input.venue_id(), input.logical_account_id(), execution->trade_id});
    if (existing->original_disposition != Disposition::BufferedGap || retained_trade == nullptr) {
      return {std::nullopt,
              model::DomainError::create_at_field(Code::InvalidPrivateOmsState,
                                                  "private_gap_commit.duplicate_baseline"),
              Reason::ArithmeticOrStateCapacityFailure, resolution};
    }
    trade = retained_trade->semantic_value;
  } else {
    auto first = derive_first_seen_authoritative_identity_plan(semantic);
    if (!first) {
      return {};
    }
    const auto* correlated =
        std::get_if<KnownFirstSeenPrivateCorrelationPlan>(&first.value().correlation_plan());
    const auto* candidate_trade =
        std::get_if<FirstSeenPrivateTradeIdentityPlan>(&first.value().trade_plan());
    if (candidate_trade == nullptr) {
      return {};
    }
    resolution = std::visit([](const auto& correlation) { return correlation.resolution; },
                            first.value().correlation_plan());
    if (const auto* prior_trade = find_committed_trade_identity(candidate_trade->key)) {
      const bool exact = prior_trade->semantic_value == candidate_trade->semantic_value;
      return {std::nullopt,
              model::DomainError::create_at_field(exact ? Code::ReconciliationIncomplete
                                                        : Code::PrivateEventConflict,
                                                  exact ? "private_gap_commit.trade_repeat_pending"
                                                        : "private_gap_commit.trade_conflict"),
              exact ? Reason::IncompleteReconciliation : Reason::TradeIdentityConflict, resolution};
    }
    // Let the preparation store compare trades it already owns. Otherwise compare the complete
    // tuple that reached trade classification before an earlier turn fell back to retention.
    for (std::uint32_t index = 0U; index < retention_.preparations.trade_record_count(); ++index) {
      if (retention_.preparations.trade_record_at(index)->key == candidate_trade->key) {
        return {};
      }
    }
    for (std::size_t index = 0U; index < retained_count; ++index) {
      const auto& prior = retention_.turns[index].execution_progress.trade;
      if (prior && prior->key == candidate_trade->key) {
        const bool exact = prior->semantic_value == candidate_trade->semantic_value;
        return {std::nullopt,
                model::DomainError::create_at_field(
                    exact ? Code::ReconciliationIncomplete : Code::PrivateEventConflict,
                    exact ? "private_gap_commit.retained_trade_repeat"
                          : "private_gap_commit.retained_trade_conflict"),
                exact ? Reason::IncompleteReconciliation : Reason::TradeIdentityConflict,
                resolution};
      }
    }
    progress.trade =
        PreparedPrivateTradeRecord{candidate_trade->key, candidate_trade->semantic_value};
    if (correlated == nullptr || first.value().preliminary_safety_reason()) {
      return {std::nullopt, std::nullopt, std::nullopt, resolution, progress};
    }

    // Earlier trade classification suppresses contradictory or redundant trade candidates.
    // Distinct, tentatively correlated facts must also respect prior one-to-one mapping claims.
    if (correlated->candidate_mapping) {
      const auto& candidate = *correlated->candidate_mapping;
      for (std::size_t index = 0U; index < retained_count; ++index) {
        const auto& prior = retention_.turns[index].execution_progress.mapping;
        if (prior && has_conflicting_mapping_claim(*prior, candidate)) {
          return {std::nullopt,
                  model::DomainError::create_at_field(
                      Code::PrivateEventConflict, "private_gap_commit.retained_mapping_conflict"),
                  Reason::CorrelationConflict, resolution, progress};
        }
      }
    }
    trade = candidate_trade->semantic_value;
    identity.emplace(std::move(first).value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve the retained order and held reservation without applying an economic change.
  const auto* known = resolution->known_resolution();
  const auto* order =
      known == nullptr ? nullptr : owner_->outbound_oms().find_order(known->order_id);
  if (order == nullptr) {
    return {std::nullopt,
            model::DomainError::create_at_field(Code::InvalidPrivateOmsState,
                                                "private_gap_commit.missing_order"),
            Reason::ArithmeticOrStateCapacityFailure, resolution, progress};
  }
  const auto before = order->private_projection();
  const auto* held = owner_->reservations().find_reservation(order->reservation_id());
  if (held == nullptr || held->state != risk::ReservationState::Held ||
      held->cumulative_confirmed_exposure.quantity != before.cumulative_filled_quantity ||
      held->exposure != order->admission().exposure || held->side != order->economics().side) {
    return {std::nullopt,
            model::DomainError::create_at_field(Code::InvalidReservationConversion,
                                                "private_gap_commit.reservation_baseline"),
            Reason::ArithmeticOrStateCapacityFailure, resolution, progress};
  }
  std::optional<oms::PrivateOmsTransitionPlan> transition;
  if (identity) {
    const auto* route = owner_->routes().find_route(order->provenance().route_id);
    if (route == nullptr) {
      return {std::nullopt,
              model::DomainError::create_at_field(Code::InvalidPrivateOmsState,
                                                  "private_gap_commit.missing_route"),
              Reason::ArithmeticOrStateCapacityFailure, resolution, progress};
    }
    auto proposed = oms::derive_private_oms_transition_before_output_capacity(
        oms::PrivateOmsTransitionInputs{order->admission(),
                                        before,
                                        *route,
                                        m4_policy_,
                                        runtime_epoch_id_,
                                        input,
                                        pending_private_executions(order->order_id()),
                                        {},
                                        0U});
    if (!proposed) {
      const auto reason = account_reason_from_gap_commit_error(proposed.error().code);
      return {std::nullopt, std::move(proposed).error(), reason, resolution, progress};
    }
    if (proposed.value().safety_reason) {
      return {std::nullopt,
              model::DomainError::create_at_field(Code::InvalidPrivateOmsState,
                                                  "private_gap_commit.authoritative_conflict"),
              proposed.value().safety_reason, resolution, progress};
    }

    // The OMS planner validated the interval against the applied quantity and active gaps.
    // A verified retained interval can still contradict this one, but cannot be drained.
    const auto start =
        execution->cumulative_quantity.checked_subtract(execution->incremental_quantity).value();
    for (std::size_t index = 0U; index < retained_count; ++index) {
      const auto& prior = retention_.turns[index].execution_progress.interval;
      if (prior && prior->order_id == order->order_id() && start < prior->end &&
          prior->start < execution->cumulative_quantity) {
        return {std::nullopt,
                model::DomainError::create_at_field(Code::InvalidPrivateOmsState,
                                                    "private_gap_commit.retained_overlap"),
                Reason::AuthoritativeContradiction, resolution, progress};
      }
    }
    progress.interval =
        RetainedPrivateExecutionInterval{order->order_id(), start, execution->cumulative_quantity};
    const auto& correlation =
        std::get<KnownFirstSeenPrivateCorrelationPlan>(identity->correlation_plan());
    if (correlation.candidate_mapping) {
      const auto& candidate = *correlation.candidate_mapping;
      progress.mapping =
          PreparedPrivateMappingCandidate{candidate.exchange_order_key, candidate.order_id};
    }
    if (proposed.value().classification != oms::ProposedPrivateOmsClassification::BufferedGap) {
      return {std::nullopt, std::nullopt, std::nullopt, resolution, progress};
    }
    // A gap proposes one effect and zero callbacks, which every installed policy can fit.
    // Validate the actual pending and identity slots before reserving journal and audit space.
    auto valid = validate_buffered_gap_state(*identity, proposed.value());
    if (!valid) {
      const auto reason = account_reason_from_gap_commit_error(valid.error().code);
      return {std::nullopt, std::move(valid).error(), reason, resolution, progress};
    }
    transition.emplace(std::move(proposed).value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reserve the actual next journal slot and its complete one-row audit span. Either lease
  // releases automatically if any later validation fails, so no number is consumed early.
  const auto disposition = transition ? Disposition::BufferedGap : Disposition::ExactEventDuplicate;
  const auto callback_decision = transition ? trace::CallbackDecision::SuppressedBuffered
                                            : trace::CallbackDecision::SuppressedDuplicate;
  const auto replay =
      recovery::JournalReplayProvenance{receipt.attempt_ordinal, receipt.receive_sequence};
  auto span =
      recovery::AuditSpan::create_audit_span(business_evidence_store_->next_audit_ordinal(), 1U);
  if (!span) {
    const auto reason = account_reason_from_gap_commit_error(span.error().code);
    return {std::nullopt, std::move(span).error(), reason, resolution, progress};
  }
  auto journal = recovery::PreparedPrivateEventJournalInput::reserve_private_event_journal_input(
      recovery_lease_, runtime_epoch_id_, input, *resolution, replay, span.value());
  if (!journal) {
    const auto reason = account_reason_from_gap_commit_error(journal.error().code);
    return {std::nullopt, std::move(journal).error(), reason, resolution, progress};
  }
  const std::array effects{trace::PrivateOrderAuditEffect{
      order->order_id(), known->provenance, before, transition ? transition->after : before, *held,
      *held, std::nullopt, std::nullopt}};
  const std::array primary{PrimaryPrivateAuditInput{input, *resolution, trade, disposition, effects,
                                                    callback_decision, std::nullopt}};
  auto evidence = business_evidence_store_->prepare_known_private_business_evidence(
      input, *resolution, trade, replay, primary, std::nullopt, {},
      journal.value().record()->sequence());
  if (!evidence) {
    const auto reason = account_reason_from_gap_commit_error(evidence.error().code);
    return {std::nullopt, std::move(evidence).error(), reason, resolution, progress};
  }
  auto journal_valid = journal.value().validate_reservation();
  if (!journal_valid) {
    const auto reason = account_reason_from_gap_commit_error(journal_valid.error().code);
    return {std::nullopt, std::move(journal_valid).error(), reason, resolution, progress};
  }
  auto evidence_valid = business_evidence_store_->validate_callback_free_publication(
      evidence.value(), *journal.value().record());
  if (!evidence_valid) {
    const auto reason = account_reason_from_gap_commit_error(evidence_valid.error().code);
    return {std::nullopt, std::move(evidence_valid).error(), reason, resolution, progress};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Construct the complete result before the first write. No phase below can fail or invoke
  // strategy code. The last store publishes both immutable records to concurrent observers.
  CallbackFreePrivateCommitResult committed{disposition, std::nullopt, std::nullopt, std::nullopt};
  journal.value().publish_validated_reservation();
  if (transition) {
    apply_validated_buffered_gap_state(*identity, *transition);
  }
  business_evidence_store_->publish_validated_callback_free_evidence(std::move(evidence).value());
  return committed;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::runtime
