// Purpose: preallocate and validate complete prospective known-order evidence, then lease immutable
// scratch without publishing journal/audit records, advancing counters, or changing business state.

#include "private_business_evidence_store.hpp"

#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace aegis::runtime {
namespace detail {

// ########################################################################
// One incarnation owns fixed future accepted slots and one unpublished scratch prefix. The scratch
// lease prevents reuse while any prepared capability survives; its shared owner keeps every copied
// fact and buffer alive after the store handle dies. Accepted prefixes remain empty in this slice.
struct PrivateBusinessEvidenceBacking {

  // --------------------------------------------------------
  // Install preallocated nested pools and allocate all fixed event, audit, and scratch slots cold.
  PrivateBusinessEvidenceBacking(
      M4Policy policy_value, recovery::RecoveryLineageId lineage_value,
      recovery::RuntimeEpochId epoch_value, recovery::AuditOrdinal next_audit_value,
      std::vector<std::shared_ptr<trace::PrivateOrderAuditEffectBuffer>> primary_buffers_value,
      std::vector<std::shared_ptr<trace::OrderCallbackAuditBuffer>> callback_buffers_value)
      : policy{std::move(policy_value)}, lineage{lineage_value}, epoch{epoch_value},
        next_audit{next_audit_value},
        event_slots(static_cast<std::size_t>(policy.capacities().max_private_event_records)),
        audit_slots(static_cast<std::size_t>(policy.capacities().max_private_audit_records)),
        primary_buffers{std::move(primary_buffers_value)},
        callback_buffers{std::move(callback_buffers_value)},
        scratch_primary_rows(static_cast<std::size_t>(
            1U + policy.capacities().max_pending_fill_intervals_per_order)) {}

  // --------------------------------------------------------
  // Preserve cold capacities separately from the detached prefix so scratch never looks accepted.
  M4Policy policy;
  recovery::RecoveryLineageId lineage;
  recovery::RuntimeEpochId epoch;
  recovery::AuditOrdinal next_audit;
  std::vector<std::optional<trace::PrivateEventEvidence>> event_slots;
  std::vector<std::optional<trace::OrderAuditRecord>> audit_slots;
  std::vector<std::shared_ptr<trace::PrivateOrderAuditEffectBuffer>> primary_buffers;
  std::vector<std::shared_ptr<trace::OrderCallbackAuditBuffer>> callback_buffers;
  std::optional<trace::PrivateEventEvidence> scratch_event;
  std::optional<recovery::PrivateEventInputJournalPayload> scratch_journal_payload;
  std::optional<recovery::AuditSpan> scratch_span;
  std::vector<std::optional<trace::OrderAuditRecord>> scratch_primary_rows;
  std::optional<trace::OrderAuditRecord> scratch_planned_callback_row;
  std::uint32_t scratch_primary_count{0U};
  std::uint32_t scratch_callback_count{0U};
  bool scratch_leased{false};
};

// ########################################################################

} // namespace detail

namespace {

// --------------------------------------------------------
// Report one exact failed evidence boundary without disguising it as business application.
[[nodiscard]] model::Result<void> reject_private_business_evidence_at_field(const char* field) {
  return model::Result<void>::create_failure(
      model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent, field));
}

// --------------------------------------------------------
// Identify the finite disposition vocabulary before applying its callback or projection profile.
[[nodiscard]] bool
is_assigned_private_event_disposition(oms::PrivateEventDisposition disposition) noexcept {
  switch (disposition) {
  case oms::PrivateEventDisposition::Applied:
  case oms::PrivateEventDisposition::BufferedGap:
  case oms::PrivateEventDisposition::AppliedFromBuffer:
  case oms::PrivateEventDisposition::ProjectionOnly:
  case oms::PrivateEventDisposition::ExactEventDuplicate:
  case oms::PrivateEventDisposition::ExactTradeDuplicate:
  case oms::PrivateEventDisposition::SafetyContained:
  case oms::PrivateEventDisposition::ForbiddenRejected:
    return true;
  }
  return false;
}

// --------------------------------------------------------
// Classify the callback-bearing known-order partition without claiming callback delivery.
[[nodiscard]] bool
does_private_disposition_propose_order_callback(oms::PrivateEventDisposition disposition) noexcept {
  return disposition == oms::PrivateEventDisposition::Applied ||
         disposition == oms::PrivateEventDisposition::AppliedFromBuffer ||
         disposition == oms::PrivateEventDisposition::ProjectionOnly ||
         disposition == oms::PrivateEventDisposition::SafetyContained;
}

// --------------------------------------------------------
// Borrow an explicit raw local locator from either an authoritative or locally minted payload.
// Interesting syntax: requires-expressions select only fields owned by each closed payload type.
[[nodiscard]] const model::OrderId*
find_private_evidence_local_locator(const oms::NormalizedPrivateOrderInput& input) noexcept {
  return std::visit(
      [](const auto& payload) -> const model::OrderId* {
        if constexpr (requires { payload.locator.local_order_id(); }) {
          return payload.locator.local_order_id() ? &*payload.locator.local_order_id() : nullptr;
        } else if constexpr (requires { payload.local_order_locator; }) {
          return payload.local_order_locator ? &*payload.local_order_locator : nullptr;
        } else if constexpr (requires { payload.order_id; }) {
          return &payload.order_id;
        } else {
          return nullptr;
        }
      },
      input.payload());
}

// --------------------------------------------------------
// Borrow an explicit source exchange locator without inventing one from local owner state.
[[nodiscard]] const oms::ExchangeOrderId*
find_private_evidence_exchange_locator(const oms::NormalizedPrivateOrderInput& input) noexcept {
  return std::visit(
      [](const auto& payload) -> const oms::ExchangeOrderId* {
        if constexpr (requires { payload.locator.exchange_order_id(); }) {
          return payload.locator.exchange_order_id() ? &*payload.locator.exchange_order_id()
                                                     : nullptr;
        } else if constexpr (requires { payload.exchange_order_id; }) {
          return &payload.exchange_order_id;
        } else {
          return nullptr;
        }
      },
      input.payload());
}

// --------------------------------------------------------
// Check sealed known ownership and source scope while retaining the original normalized subject.
[[nodiscard]] model::Result<void>
validate_known_private_evidence_source(const oms::NormalizedPrivateOrderInput& input,
                                       const oms::PrivateEventResolution& resolution,
                                       const model::M4RootProvenance& root) {
  const auto* known = resolution.known_resolution();
  if (known == nullptr || input.subject_scope() != oms::PrivateEventSubjectScope::Order ||
      !known->provenance.subject()) {
    return reject_private_business_evidence_at_field("private_business_evidence.known_resolution");
  }
  if (input.provenance().root() != root || known->provenance.root() != root) {
    return model::Result<void>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::RecoveryProvenanceMismatch,
                                            "private_business_evidence.root_provenance"));
  }
  const auto& subject = *known->provenance.subject();
  const auto* local_locator = find_private_evidence_local_locator(input);
  if (subject.logical_account_id() != input.logical_account_id() ||
      subject.venue_id() != input.venue_id() || !input.provenance().subject() ||
      (local_locator != nullptr && *local_locator != known->order_id) ||
      (input.provenance().subject()->firm_id() &&
       input.provenance().subject()->firm_id() != subject.firm_id())) {
    return reject_private_business_evidence_at_field("private_business_evidence.source_subject");
  }
  const auto& source_subject = *input.provenance().subject();
  if ((source_subject.desk_id() && source_subject.desk_id() != subject.desk_id()) ||
      (source_subject.bot_id() && source_subject.bot_id() != subject.bot_id()) ||
      (source_subject.strategy_id() && source_subject.strategy_id() != subject.strategy_id()) ||
      (source_subject.route() && source_subject.route() != subject.route())) {
    return reject_private_business_evidence_at_field(
        "private_business_evidence.source_attribution");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Require seven-scope economic prefixes to agree exactly at each buffered application boundary.
[[nodiscard]] bool
are_private_inventory_prefixes_contiguous(const trace::PrivateOrderAuditEffect& previous,
                                          const trace::PrivateOrderAuditEffect& next) noexcept {
  if (!previous.inventory_effects || !next.inventory_effects) {
    return false;
  }
  for (std::size_t index = 0U; index < previous.inventory_effects->size(); ++index) {
    const auto& before = (*previous.inventory_effects)[index];
    const auto& after = (*next.inventory_effects)[index];
    if (before.firm_id != after.firm_id || before.subject != after.subject ||
        before.instrument_id != after.instrument_id ||
        before.quote_currency != after.quote_currency || before.after != after.before) {
      return false;
    }
  }
  return true;
}

// --------------------------------------------------------
// Require the optional trade projection to describe exactly this execution and known economic side.
// A known source-side contradiction stops before trade derivation and preserves deliberate absence.
[[nodiscard]] model::Result<void>
validate_private_evidence_trade_projection(const PrimaryPrivateAuditInput& row,
                                           const trace::PrivateOrderAuditEffect& effect) {
  const auto* execution = std::get_if<oms::ExecutionPayload>(&row.input.payload());
  if (execution == nullptr) {
    return row.trade_semantic_value
               ? reject_private_business_evidence_at_field("private_business_evidence.nontrade")
               : model::Result<void>::create_success();
  }
  const auto side = effect.reservation_before.side;
  if (execution->source_side && *execution->source_side != side) {
    if ((row.disposition != oms::PrivateEventDisposition::SafetyContained &&
         row.disposition != oms::PrivateEventDisposition::ExactEventDuplicate) ||
        row.trade_semantic_value ||
        (row.disposition == oms::PrivateEventDisposition::SafetyContained &&
         (!effect.account_safety_transition ||
          effect.account_safety_transition->reason !=
              risk::AccountSafetyReason::AuthoritativeContradiction ||
          !effect.oms_after.execution_evidence_observed))) {
      return reject_private_business_evidence_at_field("private_business_evidence.source_side");
    }
    return model::Result<void>::create_success();
  }
  if (!row.trade_semantic_value) {
    return reject_private_business_evidence_at_field("private_business_evidence.trade_absence");
  }
  const auto& trade = *row.trade_semantic_value;
  const auto* known_trade =
      std::get_if<oms::KnownPrivateTradeResolution>(&trade.resolution_value());
  if (known_trade == nullptr || known_trade->order_id != effect.order_id ||
      known_trade->canonical_side != side || trade.instrument_id() != execution->instrument_id ||
      trade.metadata_revision() != execution->metadata_revision ||
      trade.incremental_quantity() != execution->incremental_quantity ||
      trade.cumulative_quantity() != execution->cumulative_quantity ||
      trade.execution_price() != execution->execution_price) {
    return reject_private_business_evidence_at_field("private_business_evidence.trade_projection");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Match each disposition to its proposed economic and callback profile before copying any scratch.
[[nodiscard]] model::Result<void>
validate_private_evidence_disposition_frame(const PrimaryPrivateAuditInput& row,
                                            const trace::PrivateOrderAuditEffect& effect) {
  const bool proposes_callback = does_private_disposition_propose_order_callback(row.disposition);
  const auto expected_suppression =
      row.disposition == oms::PrivateEventDisposition::BufferedGap
          ? trace::CallbackDecision::SuppressedBuffered
      : (row.disposition == oms::PrivateEventDisposition::ExactEventDuplicate ||
         row.disposition == oms::PrivateEventDisposition::ExactTradeDuplicate)
          ? trace::CallbackDecision::SuppressedDuplicate
          : trace::CallbackDecision::None;
  if (row.callback_decision != expected_suppression &&
      !(proposes_callback && row.callback_decision == trace::CallbackDecision::SuppressedReplay)) {
    return reject_private_business_evidence_at_field("private_business_evidence.primary_callback");
  }
  const bool applies = row.disposition == oms::PrivateEventDisposition::Applied ||
                       row.disposition == oms::PrivateEventDisposition::AppliedFromBuffer;
  const bool duplicate = row.disposition == oms::PrivateEventDisposition::ExactEventDuplicate ||
                         row.disposition == oms::PrivateEventDisposition::ExactTradeDuplicate;
  // Every known execution remains observed even when comparison suppresses its economics.
  if (row.input.kind() == oms::PrivateOrderEventKind::Execution &&
      !effect.oms_after.execution_evidence_observed) {
    return reject_private_business_evidence_at_field(
        "private_business_evidence.execution_observation");
  }
  if (!applies &&
      (effect.reservation_before != effect.reservation_after || effect.inventory_effects)) {
    return reject_private_business_evidence_at_field(
        "private_business_evidence.nonapplying_economics");
  }
  if ((duplicate || row.disposition == oms::PrivateEventDisposition::ForbiddenRejected) &&
      (effect.oms_before != effect.oms_after || effect.account_safety_transition)) {
    return reject_private_business_evidence_at_field(
        "private_business_evidence.unchanged_projection");
  }
  if (row.disposition == oms::PrivateEventDisposition::SafetyContained &&
      !effect.account_safety_transition) {
    return reject_private_business_evidence_at_field("private_business_evidence.safety_transition");
  }
  if (row.disposition == oms::PrivateEventDisposition::ForbiddenRejected &&
      (!row.error || row.error->code() != model::DomainErrorCode::InvalidPrivateOmsState)) {
    return reject_private_business_evidence_at_field("private_business_evidence.forbidden_error");
  }
  if (effect.reservation_before != effect.reservation_after && !effect.inventory_effects) {
    return reject_private_business_evidence_at_field("private_business_evidence.missing_inventory");
  }
  // A non-execution source cannot manufacture confirmed inventory, and only a definitive
  // matching terminal source can remove a live residual. This checks evidence causality without
  // duplicating the lifecycle planner's full state-transition matrix.
  if (row.input.kind() != oms::PrivateOrderEventKind::Execution) {
    if (effect.reservation_before.cumulative_confirmed_exposure !=
        effect.reservation_after.cumulative_confirmed_exposure) {
      return reject_private_business_evidence_at_field(
          "private_business_evidence.nonexecution_confirmation");
    }
    if (effect.reservation_before != effect.reservation_after) {
      std::optional<risk::ReservationClosureCause> expected_closure;
      if (row.input.kind() == oms::PrivateOrderEventKind::ExchangeRejected) {
        expected_closure = risk::ReservationClosureCause::ExchangeRejected;
      } else if (const auto* cancellation =
                     std::get_if<oms::CancellationResultPayload>(&row.input.payload());
                 cancellation != nullptr &&
                 cancellation->result == oms::CancellationResult::Cancelled &&
                 cancellation->terminal_cumulative_quantity ==
                     effect.oms_after.cumulative_filled_quantity) {
        expected_closure = risk::ReservationClosureCause::DefinitiveCancellation;
      } else if (const auto* failure = std::get_if<oms::LocalFailurePayload>(&row.input.payload());
                 failure != nullptr &&
                 failure->certainty == oms::LocalFailureCertainty::ProvenBeforeAcceptance) {
        expected_closure = risk::ReservationClosureCause::DefiniteLocalFailure;
      }
      if (!expected_closure || effect.reservation_after.state != risk::ReservationState::Released ||
          effect.reservation_after.closure_cause != *expected_closure) {
        return reject_private_business_evidence_at_field(
            "private_business_evidence.nonexecution_release");
      }
    }
  }
  if (applies && row.input.kind() == oms::PrivateOrderEventKind::Execution) {
    const auto& execution = std::get<oms::ExecutionPayload>(row.input.payload());
    auto start = execution.cumulative_quantity.checked_subtract(execution.incremental_quantity);
    if (!start || start.value() != effect.oms_before.cumulative_filled_quantity ||
        effect.oms_after.cumulative_filled_quantity != execution.cumulative_quantity ||
        !effect.inventory_effects) {
      return reject_private_business_evidence_at_field(
          "private_business_evidence.execution_interval");
    }
    const auto& subject = *effect.provenance.subject();
    if (!subject.instrument() || subject.instrument()->instrument_id != execution.instrument_id ||
        subject.instrument()->metadata_revision != execution.metadata_revision) {
      return reject_private_business_evidence_at_field(
          "private_business_evidence.execution_subject");
    }
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Match one proposed callback to the exact primary source, known subject and applied endpoint.
[[nodiscard]] model::Result<void>
validate_private_evidence_callback(const trace::OrderCallbackAuditValue& callback,
                                   const PrimaryPrivateAuditInput& row) {
  const auto& effect = row.effects.front();
  if (callback.order_id != effect.order_id || callback.provenance != effect.provenance ||
      callback.originating_event !=
          trace::originating_event_identity_from_normalized_input(row.input)) {
    return reject_private_business_evidence_at_field("private_business_evidence.callback_subject");
  }
  const auto* execution = std::get_if<oms::ExecutionPayload>(&row.input.payload());
  if (execution == nullptr) {
    if (callback.trade_id || callback.applied_cumulative_quantity) {
      return reject_private_business_evidence_at_field(
          "private_business_evidence.callback_nontrade");
    }
  } else {
    const bool applies = row.disposition == oms::PrivateEventDisposition::Applied ||
                         row.disposition == oms::PrivateEventDisposition::AppliedFromBuffer;
    if (!callback.trade_id || *callback.trade_id != execution->trade_id ||
        (applies && (!callback.applied_cumulative_quantity ||
                     *callback.applied_cumulative_quantity != execution->cumulative_quantity)) ||
        (!applies && callback.applied_cumulative_quantity)) {
      return reject_private_business_evidence_at_field(
          "private_business_evidence.callback_execution");
    }
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Transfer one exclusive detached scratch lease without changing any owned evidence or ordinal.
PreparedPrivateBusinessEvidence::PreparedPrivateBusinessEvidence(
    PreparedPrivateBusinessEvidence&& other) noexcept
    : backing_{std::move(other.backing_)} {}

// --------------------------------------------------------
// Release any prior scratch lease before transferring the source's sole inspection authority.
PreparedPrivateBusinessEvidence&
PreparedPrivateBusinessEvidence::operator=(PreparedPrivateBusinessEvidence&& other) noexcept {
  if (this != &other) {
    release_preparation();
    backing_ = std::move(other.backing_);
  }
  return *this;
}

// --------------------------------------------------------
// End detached inspection before its fixed scratch can be reused by another preparation.
PreparedPrivateBusinessEvidence::~PreparedPrivateBusinessEvidence() { release_preparation(); }

// --------------------------------------------------------
// Retain the already validated and populated scratch without allocation or duplicate lease
// creation.
PreparedPrivateBusinessEvidence::PreparedPrivateBusinessEvidence(
    std::shared_ptr<detail::PrivateBusinessEvidenceBacking> backing) noexcept
    : backing_{std::move(backing)} {}

// --------------------------------------------------------
// Clear only unpublished preparation storage before releasing its shared backing lifetime.
void PreparedPrivateBusinessEvidence::release_preparation() noexcept {
  if (backing_) {
    PrivateBusinessEvidenceStore::release_private_business_evidence_scratch(*backing_);
    backing_.reset();
  }
}

// --------------------------------------------------------
// A moved-from capability has no backing and cannot inspect a later reuse of its former scratch.
bool PreparedPrivateBusinessEvidence::has_preparation() const noexcept {
  return backing_ != nullptr;
}

// --------------------------------------------------------
// Borrow the owned prospective incoming event while the exact detached lease remains alive.
const trace::PrivateEventEvidence*
PreparedPrivateBusinessEvidence::private_event_evidence() const noexcept {
  return backing_ && backing_->scratch_event ? &*backing_->scratch_event : nullptr;
}

// --------------------------------------------------------
// Borrow the owned replay payload while preventing moved-from access to reused scratch.
const recovery::PrivateEventInputJournalPayload*
PreparedPrivateBusinessEvidence::journal_payload() const noexcept {
  return backing_ && backing_->scratch_journal_payload ? &*backing_->scratch_journal_payload
                                                       : nullptr;
}

// --------------------------------------------------------
// Borrow the checked complete proposed range without consuming its audit identities.
const recovery::AuditSpan* PreparedPrivateBusinessEvidence::audit_span() const noexcept {
  return backing_ && backing_->scratch_span ? &*backing_->scratch_span : nullptr;
}

// --------------------------------------------------------
// Report the proposed populated prefix only while this capability holds the detached lease.
std::uint32_t PreparedPrivateBusinessEvidence::primary_audit_record_count() const noexcept {
  return backing_ ? backing_->scratch_primary_count : 0U;
}

// --------------------------------------------------------
// Borrow a primary in exact cumulative application order without exposing mutable storage.
const trace::OrderAuditRecord* PreparedPrivateBusinessEvidence::primary_audit_record_at(
    std::uint32_t chronological_index) const noexcept {
  if (!backing_ || chronological_index >= backing_->scratch_primary_count) {
    return nullptr;
  }
  const auto& row = backing_->scratch_primary_rows[chronological_index];
  return row ? &*row : nullptr;
}

// --------------------------------------------------------
// Borrow a proposed aggregate Planned row without manufacturing a terminal decision.
const trace::OrderAuditRecord*
PreparedPrivateBusinessEvidence::planned_callback_record() const noexcept {
  return backing_ && backing_->scratch_planned_callback_row
             ? &*backing_->scratch_planned_callback_row
             : nullptr;
}

// --------------------------------------------------------
// Expose the prospective terminal position only when a callback-bearing span needs that final row.
std::optional<recovery::AuditOrdinal>
PreparedPrivateBusinessEvidence::reserved_terminal_audit_ordinal() const noexcept {
  return backing_ && backing_->scratch_callback_count != 0U
             ? std::optional{backing_->scratch_span->last_audit_ordinal()}
             : std::nullopt;
}

// --------------------------------------------------------
// Report the complete proposed fan-out without confusing it with callbacks actually entered.
std::uint32_t PreparedPrivateBusinessEvidence::order_callback_count() const noexcept {
  return backing_ ? backing_->scratch_callback_count : 0U;
}

// --------------------------------------------------------
// Allocate every accepted slot, full primary buffer, Planned pool, and scratch row cold. Allocation
// failures expose no partial owner; opaque domain identities already guarantee nonempty namespaces.
model::Result<std::unique_ptr<PrivateBusinessEvidenceStore>>
PrivateBusinessEvidenceStore::create_private_business_evidence_store(
    const M4Policy& policy, recovery::RecoveryLineageId lineage_id,
    recovery::RuntimeEpochId runtime_epoch_id) {
  using StoreResult = model::Result<std::unique_ptr<PrivateBusinessEvidenceStore>>;
  const auto& capacities = policy.capacities();
  const auto drain_width = 1U + capacities.max_pending_fill_intervals_per_order;
  if (!std::in_range<std::uint32_t>(drain_width) ||
      capacities.max_transition_effects_per_turn < drain_width ||
      capacities.max_private_audit_records < drain_width + 2U ||
      capacities.max_order_callbacks_per_turn < drain_width ||
      capacities.max_private_event_records == 0U) {
    return StoreResult::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidM4Policy, "private_business_evidence.capacities"));
  }
  auto next_audit = recovery::AuditOrdinal::from_value(1U);
  if (!next_audit) {
    return StoreResult::create_failure(std::move(next_audit).error());
  }
  try {
    std::vector<std::shared_ptr<trace::PrivateOrderAuditEffectBuffer>> primary_buffers;
    primary_buffers.reserve(static_cast<std::size_t>(capacities.max_private_audit_records));
    for (std::uint64_t index = 0U; index < capacities.max_private_audit_records; ++index) {
      primary_buffers.push_back(std::shared_ptr<trace::PrivateOrderAuditEffectBuffer>{
          new trace::PrivateOrderAuditEffectBuffer{
              static_cast<std::size_t>(capacities.max_transition_effects_per_turn)}});
    }
    const auto planned_buffer_count = capacities.max_private_audit_records / 3U;
    std::vector<std::shared_ptr<trace::OrderCallbackAuditBuffer>> callback_buffers;
    callback_buffers.reserve(static_cast<std::size_t>(planned_buffer_count));
    for (std::uint64_t index = 0U; index < planned_buffer_count; ++index) {
      callback_buffers.push_back(
          std::shared_ptr<trace::OrderCallbackAuditBuffer>{new trace::OrderCallbackAuditBuffer{
              static_cast<std::size_t>(capacities.max_order_callbacks_per_turn)}});
    }
    auto backing = std::make_shared<detail::PrivateBusinessEvidenceBacking>(
        policy, lineage_id, runtime_epoch_id, std::move(next_audit).value(),
        std::move(primary_buffers), std::move(callback_buffers));
    return StoreResult::create_success(std::unique_ptr<PrivateBusinessEvidenceStore>{
        new PrivateBusinessEvidenceStore{std::move(backing)}});
  } catch (const std::bad_alloc&) {
    return StoreResult::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidM4Policy, "private_business_evidence.capacity_allocation"));
  } catch (const std::length_error&) {
    return StoreResult::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidM4Policy, "private_business_evidence.capacity_allocation"));
  }
}

// --------------------------------------------------------
// Bind one fully allocated incarnation without introducing a later construction failure.
PrivateBusinessEvidenceStore::PrivateBusinessEvidenceStore(
    std::shared_ptr<detail::PrivateBusinessEvidenceBacking> backing) noexcept
    : backing_{std::move(backing)} {}

// --------------------------------------------------------
// Detached preparation authority retains the backing after this owner handle disappears.
PrivateBusinessEvidenceStore::~PrivateBusinessEvidenceStore() = default;

// --------------------------------------------------------
// Validate the complete known-order candidate against one fixed owner before acquiring its scratch.
// Copies become observable only after every fallible phase succeeds, and no accepted slot changes.
model::Result<PreparedPrivateBusinessEvidence>
PrivateBusinessEvidenceStore::prepare_known_private_business_evidence(
    const oms::NormalizedPrivateOrderInput& input,
    const oms::PrivateEventResolution& first_admission_resolution,
    std::optional<oms::PrivateTradeSemanticValue> trade_semantic_value,
    recovery::JournalReplayProvenance replay_provenance,
    std::span<const PrimaryPrivateAuditInput> primary_rows,
    std::optional<trace::CallbackOrdinalRange> callback_range,
    std::span<const trace::OrderCallbackAuditValue> callbacks,
    std::optional<recovery::JournalSequence> journal_sequence,
    std::optional<recovery::DiagnosticOrdinal> diagnostic_ordinal) {
  using PreparationResult = model::Result<PreparedPrivateBusinessEvidence>;
  auto& backing = *backing_;
  const auto& capacities = backing.policy.capacities();

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject lease conflicts before any candidate work can alter another detached preparation.
  if (backing.scratch_leased) {
    return PreparationResult::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::PrivateEvidenceExhausted,
                                            "private_business_evidence.preparation_lease"));
  }
  auto valid_source = validate_known_private_evidence_source(input, first_admission_resolution,
                                                             backing.policy.root_provenance());
  if (!valid_source) {
    return PreparationResult::create_failure(std::move(valid_source).error());
  }
  if (primary_rows.empty() || primary_rows.size() > backing.scratch_primary_rows.size() ||
      callbacks.size() > capacities.max_order_callbacks_per_turn) {
    return PreparationResult::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::PrivateEvidenceExhausted,
                                            "private_business_evidence.turn_capacity"));
  }
  const auto& first_row = primary_rows.front();
  if (first_row.input != input ||
      first_row.first_admission_resolution != first_admission_resolution ||
      first_row.trade_semantic_value != trade_semantic_value ||
      first_row.disposition == oms::PrivateEventDisposition::AppliedFromBuffer) {
    return PreparationResult::create_failure(
        std::move(
            reject_private_business_evidence_at_field("private_business_evidence.incoming_primary"))
            .error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate every complete primary and require truthful chained buffered executions after input.
  std::uint32_t expected_callback_count = 0U;
  for (std::size_t index = 0U; index < primary_rows.size(); ++index) {
    const auto& row = primary_rows[index];
    auto source = validate_known_private_evidence_source(row.input, row.first_admission_resolution,
                                                         backing.policy.root_provenance());
    if (!source) {
      return PreparationResult::create_failure(std::move(source).error());
    }
    if (!is_assigned_private_event_disposition(row.disposition) || row.effects.size() != 1U ||
        row.effects.size() > capacities.max_transition_effects_per_turn ||
        row.first_admission_resolution != first_admission_resolution) {
      return PreparationResult::create_failure(
          std::move(reject_private_business_evidence_at_field(
                        "private_business_evidence.primary_profile"))
              .error());
    }
    const auto& effect = row.effects.front();
    const auto& known = *first_admission_resolution.known_resolution();
    if (effect.order_id != known.order_id || effect.provenance != known.provenance) {
      return PreparationResult::create_failure(
          std::move(
              reject_private_business_evidence_at_field("private_business_evidence.effect_subject"))
              .error());
    }
    // Source-normalized instrument attribution stays independent from resolved local ownership.
    // A proved disagreement is retained only as a contained execution contradiction or duplicate.
    const auto& source_subject = *row.input.provenance().subject();
    if (source_subject.instrument() &&
        source_subject.instrument() != known.provenance.subject()->instrument()) {
      const bool duplicate = row.disposition == oms::PrivateEventDisposition::ExactEventDuplicate ||
                             row.disposition == oms::PrivateEventDisposition::ExactTradeDuplicate;
      const bool contained = row.input.kind() == oms::PrivateOrderEventKind::Execution &&
                             row.disposition == oms::PrivateEventDisposition::SafetyContained &&
                             effect.account_safety_transition &&
                             effect.account_safety_transition->reason ==
                                 risk::AccountSafetyReason::AuthoritativeContradiction;
      if (!duplicate && !contained) {
        return PreparationResult::create_failure(
            std::move(reject_private_business_evidence_at_field(
                          "private_business_evidence.source_instrument"))
                .error());
      }
    }
    if (effect.oms_before.pending_fill_count > capacities.max_pending_fill_intervals_per_order ||
        effect.oms_after.pending_fill_count > capacities.max_pending_fill_intervals_per_order ||
        effect.oms_before.cancel_attempt_count > capacities.max_cancel_attempts ||
        effect.oms_after.cancel_attempt_count > capacities.max_cancel_attempts) {
      return PreparationResult::create_failure(
          std::move(reject_private_business_evidence_at_field(
                        "private_business_evidence.oms_side_table_capacity"))
              .error());
    }
    const auto* source_exchange = find_private_evidence_exchange_locator(row.input);
    const bool adopts_exchange =
        row.disposition == oms::PrivateEventDisposition::Applied ||
        row.disposition == oms::PrivateEventDisposition::ProjectionOnly ||
        row.disposition == oms::PrivateEventDisposition::AppliedFromBuffer ||
        row.disposition == oms::PrivateEventDisposition::BufferedGap;
    if (adopts_exchange && source_exchange != nullptr &&
        ((!effect.oms_after.exchange_order_id ||
          *effect.oms_after.exchange_order_id != *source_exchange) ||
         (effect.oms_before.exchange_order_id &&
          *effect.oms_before.exchange_order_id != *source_exchange))) {
      return PreparationResult::create_failure(
          std::move(reject_private_business_evidence_at_field(
                        "private_business_evidence.exchange_projection"))
              .error());
    }
    auto frame =
        trace::validate_private_order_audit_effect(effect, backing.policy.root_provenance());
    if (!frame) {
      return PreparationResult::create_failure(std::move(frame).error());
    }
    auto trade = validate_private_evidence_trade_projection(row, effect);
    if (!trade) {
      return PreparationResult::create_failure(std::move(trade).error());
    }
    auto disposition = validate_private_evidence_disposition_frame(row, effect);
    if (!disposition) {
      return PreparationResult::create_failure(std::move(disposition).error());
    }
    if (index != 0U) {
      const auto& previous = primary_rows[index - 1U].effects.front();
      if (first_row.input.kind() != oms::PrivateOrderEventKind::Execution ||
          first_row.disposition != oms::PrivateEventDisposition::Applied ||
          row.input.kind() != oms::PrivateOrderEventKind::Execution ||
          row.disposition != oms::PrivateEventDisposition::AppliedFromBuffer ||
          effect.oms_before.pending_fill_count == 0U ||
          effect.oms_after.pending_fill_count != effect.oms_before.pending_fill_count - 1U ||
          effect.oms_before != previous.oms_after ||
          effect.reservation_before != previous.reservation_after ||
          !are_private_inventory_prefixes_contiguous(previous, effect)) {
        return PreparationResult::create_failure(
            std::move(reject_private_business_evidence_at_field(
                          "private_business_evidence.buffered_chain"))
                .error());
      }
      const auto& execution = std::get<oms::ExecutionPayload>(row.input.payload());
      for (std::size_t prior = 0U; prior < index; ++prior) {
        const auto* current_exchange = find_private_evidence_exchange_locator(row.input);
        const auto* prior_exchange =
            find_private_evidence_exchange_locator(primary_rows[prior].input);
        if (current_exchange != nullptr && prior_exchange != nullptr &&
            *current_exchange != *prior_exchange) {
          return PreparationResult::create_failure(
              std::move(reject_private_business_evidence_at_field(
                            "private_business_evidence.buffered_exchange"))
                  .error());
        }
        if (oms::PrivateEventRegistryKey::from_ingress_semantic_value(
                oms::PrivateEventIngressSemanticValue::from_normalized_input(row.input)) ==
                oms::PrivateEventRegistryKey::from_ingress_semantic_value(
                    oms::PrivateEventIngressSemanticValue::from_normalized_input(
                        primary_rows[prior].input)) ||
            execution.trade_id ==
                std::get<oms::ExecutionPayload>(primary_rows[prior].input.payload()).trade_id) {
          return PreparationResult::create_failure(
              std::move(reject_private_business_evidence_at_field(
                            "private_business_evidence.buffered_identity"))
                  .error());
        }
      }
    }
    if (does_private_disposition_propose_order_callback(row.disposition) &&
        row.callback_decision != trace::CallbackDecision::SuppressedReplay) {
      ++expected_callback_count;
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Match exact callback identity order and reserve a complete prospective terminal position.
  if (callbacks.size() != expected_callback_count ||
      callback_range.has_value() != (expected_callback_count != 0U) ||
      (callback_range && callback_range->callback_count() != expected_callback_count)) {
    return PreparationResult::create_failure(
        std::move(
            reject_private_business_evidence_at_field("private_business_evidence.callback_range"))
            .error());
  }
  std::size_t callback_index = 0U;
  for (const auto& row : primary_rows) {
    if (does_private_disposition_propose_order_callback(row.disposition) &&
        row.callback_decision != trace::CallbackDecision::SuppressedReplay) {
      auto callback = validate_private_evidence_callback(callbacks[callback_index], row);
      if (!callback) {
        return PreparationResult::create_failure(std::move(callback).error());
      }
      ++callback_index;
    }
  }
  const auto primary_count = static_cast<std::uint32_t>(primary_rows.size());
  const auto audit_count = primary_count + (expected_callback_count == 0U ? 0U : 2U);
  if (audit_count > capacities.max_private_audit_records ||
      (expected_callback_count != 0U && backing.callback_buffers.empty())) {
    return PreparationResult::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::PrivateEvidenceExhausted,
                                            "private_business_evidence.audit_capacity"));
  }
  auto span = recovery::AuditSpan::create_audit_span(backing.next_audit, audit_count);
  if (!span) {
    return PreparationResult::create_failure(std::move(span).error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy complete immutable candidates only after every failure boundary has passed. All nested
  // storage and return wrappers already exist or move without failure; accepted stores stay empty.
  static_assert(std::is_nothrow_copy_constructible_v<trace::PrivateOrderAuditEffect>);
  static_assert(std::is_nothrow_copy_constructible_v<trace::OrderCallbackAuditValue>);
  static_assert(std::is_nothrow_move_constructible_v<trace::OrderAuditRecord>);
  static_assert(std::is_nothrow_move_constructible_v<trace::PrivateEventEvidence>);
  static_assert(std::is_nothrow_move_constructible_v<PreparationResult>);
  backing.scratch_event.emplace(trace::PrivateEventEvidence{
      input, oms::PrivateEventIngressSemanticValue::from_normalized_input(input),
      first_admission_resolution, std::move(trade_semantic_value), backing.epoch,
      backing.policy.root_provenance(), replay_provenance, first_row.disposition, journal_sequence,
      diagnostic_ordinal});
  backing.scratch_journal_payload.emplace(
      recovery::PrivateEventInputJournalPayload{input, first_admission_resolution});
  backing.scratch_span.emplace(std::move(span).value());
  for (std::uint32_t index = 0U; index < primary_count; ++index) {
    const auto& row = primary_rows[index];
    auto& buffer = backing.primary_buffers[index];
    buffer->values_[0U].emplace(row.effects.front());
    const auto ordinal = recovery::AuditOrdinal::from_value(backing.next_audit.value() + index);
    backing.scratch_primary_rows[index].emplace(
        trace::OrderAuditRecord{backing.epoch,
                                ordinal.value(),
                                trace::M4AuditKind::EventDisposition,
                                trace::originating_event_identity_from_normalized_input(row.input),
                                row.input,
                                backing.policy.root_provenance(),
                                row.effects.front().provenance.subject(),
                                journal_sequence,
                                row.disposition,
                                row.callback_decision,
                                std::nullopt,
                                row.error,
                                buffer,
                                1U,
                                {},
                                0U});
  }
  if (expected_callback_count != 0U) {
    auto& buffer = backing.callback_buffers.front();
    for (std::uint32_t index = 0U; index < expected_callback_count; ++index) {
      buffer->values_[index].emplace(callbacks[index]);
    }
    const auto ordinal =
        recovery::AuditOrdinal::from_value(backing.next_audit.value() + primary_count);
    backing.scratch_planned_callback_row.emplace(
        trace::OrderAuditRecord{backing.epoch,
                                ordinal.value(),
                                trace::M4AuditKind::OrderCallbackDecision,
                                trace::OriginatingEventIdentity::create_without_originating_event(),
                                std::nullopt,
                                backing.policy.root_provenance(),
                                std::nullopt,
                                journal_sequence,
                                std::nullopt,
                                trace::CallbackDecision::Planned,
                                callback_range,
                                std::nullopt,
                                {},
                                0U,
                                buffer,
                                expected_callback_count});
  }
  backing.scratch_primary_count = primary_count;
  backing.scratch_callback_count = expected_callback_count;
  backing.scratch_leased = true;
  return PreparationResult::create_success(PreparedPrivateBusinessEvidence{backing_});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Clear every populated detached slot while preserving exact empty accepted prefixes and counters.
void PrivateBusinessEvidenceStore::release_private_business_evidence_scratch(
    detail::PrivateBusinessEvidenceBacking& backing) noexcept {
  for (std::uint32_t index = 0U; index < backing.scratch_primary_count; ++index) {
    backing.scratch_primary_rows[index].reset();
    backing.primary_buffers[index]->values_[0U].reset();
  }
  if (backing.scratch_callback_count != 0U) {
    backing.scratch_planned_callback_row.reset();
    auto& buffer = backing.callback_buffers.front();
    for (std::uint32_t index = 0U; index < backing.scratch_callback_count; ++index) {
      buffer->values_[index].reset();
    }
  }
  backing.scratch_event.reset();
  backing.scratch_journal_payload.reset();
  backing.scratch_span.reset();
  backing.scratch_primary_count = 0U;
  backing.scratch_callback_count = 0U;
  backing.scratch_leased = false;
}

// --------------------------------------------------------
// Borrow the immutable policy root without observing detached scratch as published evidence.
const model::M4RootProvenance& PrivateBusinessEvidenceStore::root_provenance() const noexcept {
  return backing_->policy.root_provenance();
}

// --------------------------------------------------------
// Borrow the lineage copied at cold construction; this accessor grants no medium lease.
const recovery::RecoveryLineageId& PrivateBusinessEvidenceStore::lineage_id() const noexcept {
  return backing_->lineage;
}

// --------------------------------------------------------
// Borrow the incarnation qualifying each proposed audit identity.
const recovery::RuntimeEpochId& PrivateBusinessEvidenceStore::runtime_epoch_id() const noexcept {
  return backing_->epoch;
}

// --------------------------------------------------------
// Report the unconsumed next ordinal, unchanged by repeated preparations or abandonment.
recovery::AuditOrdinal PrivateBusinessEvidenceStore::next_audit_ordinal() const noexcept {
  return backing_->next_audit;
}

// --------------------------------------------------------
// Report the exact event capacity allocated cold from max_private_event_records.
std::uint32_t PrivateBusinessEvidenceStore::private_event_record_capacity() const noexcept {
  return static_cast<std::uint32_t>(backing_->event_slots.size());
}

// --------------------------------------------------------
// Report exact audit physical-slot capacity, including slots needed by future terminal rows.
std::uint32_t PrivateBusinessEvidenceStore::private_audit_record_capacity() const noexcept {
  return static_cast<std::uint32_t>(backing_->audit_slots.size());
}

// --------------------------------------------------------
// Report one full primary effect backing for every possible physical primary row.
std::uint32_t PrivateBusinessEvidenceStore::primary_effect_buffer_count() const noexcept {
  return static_cast<std::uint32_t>(backing_->primary_buffers.size());
}

// --------------------------------------------------------
// Report each primary's exact cold max_transition_effects_per_turn capacity.
std::uint32_t PrivateBusinessEvidenceStore::primary_effect_buffer_capacity() const noexcept {
  return static_cast<std::uint32_t>(backing_->policy.capacities().max_transition_effects_per_turn);
}

// --------------------------------------------------------
// Report the exact topology-derived Planned pool floor(max_private_audit_records / 3).
std::uint32_t PrivateBusinessEvidenceStore::planned_callback_buffer_count() const noexcept {
  return static_cast<std::uint32_t>(backing_->callback_buffers.size());
}

// --------------------------------------------------------
// Report each Planned buffer's exact cold max_order_callbacks_per_turn capacity.
std::uint32_t PrivateBusinessEvidenceStore::planned_callback_buffer_capacity() const noexcept {
  return static_cast<std::uint32_t>(backing_->policy.capacities().max_order_callbacks_per_turn);
}

// --------------------------------------------------------
// Preparation has no acceptance operation, so the retained accepted event prefix remains empty.
std::uint32_t PrivateBusinessEvidenceStore::accepted_private_event_record_count() const noexcept {
  return 0U;
}

// --------------------------------------------------------
// No prospective primary or Planned row becomes accepted in this preparation-only store.
std::uint32_t PrivateBusinessEvidenceStore::accepted_private_audit_record_count() const noexcept {
  return 0U;
}

// --------------------------------------------------------
// Report whether a detached capability prevents scratch reuse by a later preparation.
bool PrivateBusinessEvidenceStore::has_outstanding_preparation() const noexcept {
  return backing_->scratch_leased;
}

// --------------------------------------------------------

} // namespace aegis::runtime
