// Purpose: implement one-source deterministic continuity, readiness, transactional commit, and
// prevalidated runtime-trace publication without performing strategy dispatch.

#include "aegis/market_data/market_state_machine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace aegis::market_data {
namespace {

// ########################################################################
// Standalone market-state users opt into a stateless authority that accepts the internally proven
// bounded shape without creating a strategy-dispatch plan.
class PermissiveMarketTurnPreflightAuthority final : public MarketTurnPreflightAuthority {
public:

  // --------------------------------------------------------
  // Accept every already validated exact request without retaining owner-turn state.
  [[nodiscard]] model::Result<void>
  authorize_market_turn(const MarketTurnPreflight& preflight) override {
    static_cast<void>(preflight);
    return model::Result<void>::create_success();
  }

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// MarketTurnTraceDraftBatch retains the at-most-two state-machine records on the stack so every
// shape and the complete callback/re-entry fan-out can be preflighted before append or domain
// mutation.
struct MarketTurnTraceDraftBatch {

  // ########################################################################
  // One fixed draft pairs its schema kind with the complete fields validated before append.
  struct MarketTurnTraceDraft {
    trace::RuntimeTraceEventKind kind;
    trace::RuntimeTraceFields fields;
  };

  // ########################################################################

  // --------------------------------------------------------
  // Append one draft into the fixed bound established by the M2 event vocabulary.
  void append_trace_draft(trace::RuntimeTraceEventKind kind, trace::RuntimeTraceFields fields) {
    entries[count] = MarketTurnTraceDraft{kind, std::move(fields)};
    ++count;
  }

  // --------------------------------------------------------
  std::array<std::optional<MarketTurnTraceDraft>, 2U> entries{};
  std::size_t count{0U};
};

// ########################################################################

// ########################################################################
// CallbackFanoutBudget fixes exact callbacks and the paired callback/re-entry trace slots derived
// from the actually published event set.
struct CallbackFanoutBudget {
  std::uint32_t callback_count;
  std::uint32_t callback_trace_records;
};

// ########################################################################

// --------------------------------------------------------
// Construct one stable field-level failure without retaining caller-authored text.
[[nodiscard]] model::Result<void> create_market_state_failure_result(model::DomainErrorCode code,
                                                                     std::string field) {
  return model::Result<void>::create_failure(
      model::DomainError::create_at_field(code, std::move(field)));
}

// --------------------------------------------------------
// Lift a domain error into the result type needed by one public state-machine operation.
template <typename Value>
[[nodiscard]] model::Result<Value> propagate_market_state_failure(const model::DomainError& error) {
  return model::Result<Value>::create_failure(error);
}

// --------------------------------------------------------
// Map the market boundary's four states to the independently assigned trace vocabulary.
[[nodiscard]] trace::RuntimeMarketState
runtime_trace_state_from_market_readiness(MarketReadiness readiness) noexcept {
  switch (readiness) {
  case MarketReadiness::Synchronizing:
    return trace::RuntimeMarketState::Synchronizing;
  case MarketReadiness::Ready:
    return trace::RuntimeMarketState::Ready;
  case MarketReadiness::Stale:
    return trace::RuntimeMarketState::Stale;
  case MarketReadiness::Invalid:
    return trace::RuntimeMarketState::Invalid;
  default:
    return trace::RuntimeMarketState::Unspecified;
  }
}

// --------------------------------------------------------
// Attach a complete committed book identity when one exists, including while its bytes are hidden
// by Synchronizing or Invalid readiness.
void attach_book_identity(trace::RuntimeTraceFields& fields,
                          const std::optional<BookIdentity>& identity) noexcept {
  if (identity) {
    fields.book_generation = identity->generation();
    fields.book_revision = identity->revision();
  }
}

// --------------------------------------------------------
// Attach the same book identity to a sanitized strategy-facing state transition.
void attach_book_identity(MarketStateEventFields& fields,
                          const std::optional<BookIdentity>& identity) noexcept {
  if (identity) {
    fields.book_generation = identity->generation();
    fields.book_revision = identity->revision();
  }
}

// --------------------------------------------------------
// Derive callback count from actual event presence and prove both it and paired trace reservations
// fit policy and fixed-width arithmetic.
[[nodiscard]] model::Result<CallbackFanoutBudget>
calculate_fanout_budget(std::uint32_t matching_subscriptions, std::uint32_t event_count,
                        std::uint32_t maximum_callbacks_per_turn) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Multiply in a wider domain so hostile counts cannot wrap before the policy comparison.
  const auto callbacks = static_cast<std::uint64_t>(matching_subscriptions) * event_count;
  if (callbacks > maximum_callbacks_per_turn ||
      callbacks > std::numeric_limits<std::uint32_t>::max()) {
    return model::Result<CallbackFanoutBudget>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::DispatchCapacityExceeded, "market_turn.callbacks"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Every callback needs its own canonical callback record plus one possible first re-entry record.
  const auto trace_records = callbacks * 2U;
  if (trace_records > std::numeric_limits<std::uint32_t>::max()) {
    return model::Result<CallbackFanoutBudget>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::DispatchCapacityExceeded, "market_turn.callback_trace_records"));
  }
  return model::Result<CallbackFanoutBudget>::create_success(CallbackFanoutBudget{
      static_cast<std::uint32_t>(callbacks), static_cast<std::uint32_t>(trace_records)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Authorize exact callback fan-out, preflight complete evidence, validate every fixed-field draft,
// and append the proven prefix before the following no-fail domain commit.
[[nodiscard]] model::Result<void>
append_trace_drafts(MarketTurnTraceDraftBatch& drafts, model::MarketSourceOrdinal source_ordinal,
                    model::TurnOrdinal turn_ordinal, std::uint32_t event_count,
                    std::uint32_t matching_subscription_count, const CallbackFanoutBudget& fanout,
                    MarketTurnPreflightAuthority& preflight_authority,
                    trace::RuntimeTraceSink& trace_sink) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Combine the fixed two-record bound and exact callback reserve without unsigned wrap.
  const auto state_machine_records = static_cast<std::uint32_t>(drafts.count);
  if (fanout.callback_trace_records >
      std::numeric_limits<std::uint32_t>::max() - state_machine_records) {
    return create_market_state_failure_result(model::DomainErrorCode::TraceCapacityExceeded,
                                              "runtime_trace.turn_records");
  }
  const auto total_trace_records = state_machine_records + fanout.callback_trace_records;

  // ++++++++++++++++++++++++++++++++++++++++
  // Give the coordinator one market-data-neutral exact shape after classification and event
  // shaping, while the current book, continuity, readiness, and canonical trace prefix are still
  // unchanged.
  auto authorized = preflight_authority.authorize_market_turn(MarketTurnPreflight{
      source_ordinal, turn_ordinal, event_count, matching_subscription_count, fanout.callback_count,
      state_machine_records, fanout.callback_trace_records, total_trace_records});
  if (!authorized) {
    return authorized;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reserve the entire state-machine and callback/re-entry evidence set as one exact turn.
  auto capacity = trace_sink.preflight_trace_append(total_trace_records);
  if (!capacity) {
    return capacity;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove all shapes and source membership before the first accepted-prefix mutation.
  for (std::size_t index = 0U; index < drafts.count; ++index) {
    auto validation = trace_sink.validate_trace_record(drafts.entries[index]->kind,
                                                       drafts.entries[index]->fields);
    if (!validation) {
      return validation;
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Append cannot fail after exact preflight and identical validation under the serialized owner.
  for (std::size_t index = 0U; index < drafts.count; ++index) {
    auto& draft = drafts.entries[index].value();
    auto appended = trace_sink.append_trace_record(draft.kind, std::move(draft.fields));
    if (!appended) {
      return appended;
    }
  }
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Require an initialized explicit readiness before any ordinary market or control turn.
[[nodiscard]] model::Result<void>
require_initialized(const std::optional<MarketReadiness>& readiness) {
  if (!readiness) {
    return create_market_state_failure_result(model::DomainErrorCode::MarketNotReady,
                                              "market_state.initialization");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Reject a source tuple that was not minted from the same configured policy entry as this owner.
[[nodiscard]] model::Result<void> require_source(const MarketSourceIdentity& expected,
                                                 const MarketSourceIdentity& actual) {
  if (expected != actual) {
    return create_market_state_failure_result(model::DomainErrorCode::RuntimeSourceNotConfigured,
                                              "market_state.source");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Reject a trace authority that was not sealed by the complete policy that created this owner.
[[nodiscard]] model::Result<void>
require_trace_provenance(const trace::RuntimeTraceProvenance& expected,
                         const trace::RuntimeTraceSink& trace_sink) {
  if (trace_sink.provenance() != expected) {
    return create_market_state_failure_result(model::DomainErrorCode::InvalidRelationship,
                                              "market_state.trace_provenance");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Validate owner processing time against one accepted envelope before trace or state can change.
[[nodiscard]] model::Result<void> require_processing_order(model::ProcessingTimestamp processing,
                                                           model::ReceiveTimestamp receive) {
  auto delay = model::calculate_processing_delay(processing, receive);
  if (!delay) {
    return create_market_state_failure_result(model::DomainErrorCode::InvalidMarketEvent,
                                              "market_turn.processing_timestamp");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Construct a parsed-update input-disposition record with the intended resulting state/book.
[[nodiscard]] trace::RuntimeTraceFields create_update_input_trace_fields(
    const trace::RuntimeTraceSource& source, const NormalizedMarketUpdate& update,
    const AcceptedMarketTurnContext& context, trace::RuntimeInputDisposition disposition,
    MarketReadiness resulting_state, const std::optional<BookIdentity>& identity) {
  trace::RuntimeTraceFields fields;
  fields.source = source;
  fields.admission_ordinal = context.admission_ordinal;
  fields.turn_ordinal = context.turn_ordinal;
  fields.session_epoch = update.session_epoch();
  fields.source_sequence = update.source_sequence();
  fields.receive_sequence = update.receive_sequence();
  fields.metadata_revision = update.metadata_revision();
  fields.input_disposition = disposition;
  fields.state = runtime_trace_state_from_market_readiness(resulting_state);
  attach_book_identity(fields, identity);
  return fields;
}

// --------------------------------------------------------
// Construct an accepted-envelope input record for session, staleness, or sanitized parse controls.
[[nodiscard]] trace::RuntimeTraceFields create_envelope_input_trace_fields(
    const trace::RuntimeTraceSource& source, model::SessionEpoch session_epoch,
    model::ReceiveSequence receive_sequence, const AcceptedMarketTurnContext& context,
    trace::RuntimeInputDisposition disposition, MarketReadiness resulting_state,
    const std::optional<BookIdentity>& identity) {
  trace::RuntimeTraceFields fields;
  fields.source = source;
  fields.admission_ordinal = context.admission_ordinal;
  fields.turn_ordinal = context.turn_ordinal;
  fields.session_epoch = session_epoch;
  fields.receive_sequence = receive_sequence;
  fields.input_disposition = disposition;
  fields.state = runtime_trace_state_from_market_readiness(resulting_state);
  attach_book_identity(fields, identity);
  return fields;
}

// --------------------------------------------------------
// Construct the attempt-only input record emitted when the owner consumes an admission-loss fence.
[[nodiscard]] trace::RuntimeTraceFields create_discontinuity_input_trace_fields(
    const trace::RuntimeTraceSource& source, model::AdmissionOrdinal admission_ordinal,
    model::TurnOrdinal turn_ordinal, MarketReadiness resulting_state,
    const std::optional<BookIdentity>& identity) {
  trace::RuntimeTraceFields fields;
  fields.source = source;
  fields.admission_ordinal = admission_ordinal;
  fields.turn_ordinal = turn_ordinal;
  fields.input_disposition = trace::RuntimeInputDisposition::SourceDiscontinuity;
  fields.state = runtime_trace_state_from_market_readiness(resulting_state);
  attach_book_identity(fields, identity);
  return fields;
}

// --------------------------------------------------------
// Construct a parsed-update state-transition record after the event profile has been validated.
[[nodiscard]] trace::RuntimeTraceFields create_update_transition_trace_fields(
    const trace::RuntimeTraceSource& source, const NormalizedMarketUpdate& update,
    const AcceptedMarketTurnContext& context, MarketReadiness previous, MarketReadiness next,
    const std::optional<BookIdentity>& identity) {
  auto fields = create_update_input_trace_fields(
      source, update, context, trace::RuntimeInputDisposition::SnapshotApplied, next, identity);
  fields.input_disposition = trace::RuntimeInputDisposition::Unspecified;
  fields.previous_state = runtime_trace_state_from_market_readiness(previous);
  return fields;
}

// --------------------------------------------------------
// Construct an accepted-envelope transition record for state-changing control input.
[[nodiscard]] trace::RuntimeTraceFields create_envelope_transition_trace_fields(
    const trace::RuntimeTraceSource& source, model::SessionEpoch session_epoch,
    model::ReceiveSequence receive_sequence, const AcceptedMarketTurnContext& context,
    MarketReadiness previous, MarketReadiness next, const std::optional<BookIdentity>& identity) {
  auto fields = create_envelope_input_trace_fields(source, session_epoch, receive_sequence, context,
                                                   trace::RuntimeInputDisposition::SessionReset,
                                                   next, identity);
  fields.input_disposition = trace::RuntimeInputDisposition::Unspecified;
  fields.previous_state = runtime_trace_state_from_market_readiness(previous);
  return fields;
}

// --------------------------------------------------------
// Construct an owner-control transition without fabricated admission or market context.
[[nodiscard]] trace::RuntimeTraceFields
create_owner_transition_trace_fields(const trace::RuntimeTraceSource& source,
                                     const OwnerMarketTurnContext& context,
                                     std::optional<MarketReadiness> previous, MarketReadiness next,
                                     const std::optional<BookIdentity>& identity) {
  trace::RuntimeTraceFields fields;
  fields.source = source;
  fields.turn_ordinal = context.turn_ordinal;
  fields.previous_state = previous ? runtime_trace_state_from_market_readiness(*previous)
                                   : trace::RuntimeMarketState::Unspecified;
  fields.state = runtime_trace_state_from_market_readiness(next);
  attach_book_identity(fields, identity);
  return fields;
}

// --------------------------------------------------------
// Construct the attempt-only transition produced by an attributable discontinuity fence.
[[nodiscard]] trace::RuntimeTraceFields create_discontinuity_transition_trace_fields(
    const trace::RuntimeTraceSource& source, model::AdmissionOrdinal admission_ordinal,
    model::TurnOrdinal turn_ordinal, MarketReadiness previous,
    const std::optional<BookIdentity>& identity) {
  auto fields = create_discontinuity_input_trace_fields(source, admission_ordinal, turn_ordinal,
                                                        MarketReadiness::Invalid, identity);
  fields.input_disposition = trace::RuntimeInputDisposition::Unspecified;
  fields.previous_state = runtime_trace_state_from_market_readiness(previous);
  return fields;
}

// --------------------------------------------------------
// Build the strategy-facing full-update transition profile used for Ready and Invalid destinations.
[[nodiscard]] MarketStateEventFields create_update_state_event_fields(
    const MarketSourceIdentity& source, const NormalizedMarketUpdate& update,
    const AcceptedMarketTurnContext& context, MarketReadiness previous, MarketReadiness next,
    const std::optional<BookIdentity>& identity) {
  MarketStateEventFields fields{source,
                                update.session_epoch(),
                                update.source_sequence(),
                                update.receive_sequence(),
                                update.receive_timestamp(),
                                context.admission_ordinal,
                                context.turn_ordinal,
                                context.processing_timestamp,
                                update.metadata_revision(),
                                std::nullopt,
                                std::nullopt,
                                previous,
                                next};
  attach_book_identity(fields, identity);
  return fields;
}

// --------------------------------------------------------
// Build the strategy-facing accepted-envelope transition profile used by control input.
[[nodiscard]] MarketStateEventFields create_envelope_state_event_fields(
    const MarketSourceIdentity& source, model::SessionEpoch session_epoch,
    model::ReceiveSequence receive_sequence, model::ReceiveTimestamp receive_timestamp,
    const AcceptedMarketTurnContext& context, MarketReadiness previous, MarketReadiness next,
    const std::optional<BookIdentity>& identity) {
  MarketStateEventFields fields{source,
                                session_epoch,
                                std::nullopt,
                                receive_sequence,
                                receive_timestamp,
                                context.admission_ordinal,
                                context.turn_ordinal,
                                context.processing_timestamp,
                                std::nullopt,
                                std::nullopt,
                                std::nullopt,
                                previous,
                                next};
  attach_book_identity(fields, identity);
  return fields;
}

// --------------------------------------------------------
// Build the owner-control strategy event used by initialization and explicit resynchronization.
[[nodiscard]] MarketStateEventFields
create_owner_state_event_fields(const MarketSourceIdentity& source,
                                const OwnerMarketTurnContext& context,
                                std::optional<MarketReadiness> previous, MarketReadiness next,
                                const std::optional<BookIdentity>& identity) {
  MarketStateEventFields fields{
      source,       std::nullopt, std::nullopt,         std::nullopt,
      std::nullopt, std::nullopt, context.turn_ordinal, context.processing_timestamp,
      std::nullopt, std::nullopt, std::nullopt,         previous,
      next};
  attach_book_identity(fields, identity);
  return fields;
}

// --------------------------------------------------------
// Build the attempt-only strategy event produced by a source discontinuity fence.
[[nodiscard]] MarketStateEventFields create_discontinuity_state_event_fields(
    const MarketSourceIdentity& source, model::AdmissionOrdinal admission_ordinal,
    const OwnerMarketTurnContext& context, MarketReadiness previous,
    const std::optional<BookIdentity>& identity) {
  MarketStateEventFields fields{source,
                                std::nullopt,
                                std::nullopt,
                                std::nullopt,
                                std::nullopt,
                                admission_ordinal,
                                context.turn_ordinal,
                                context.processing_timestamp,
                                std::nullopt,
                                std::nullopt,
                                std::nullopt,
                                previous,
                                MarketReadiness::Invalid};
  attach_book_identity(fields, identity);
  return fields;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Share one immutable permissive authority across standalone callers without hidden owner state.
MarketTurnPreflightAuthority& permissive_market_turn_preflight_authority() noexcept {
  static PermissiveMarketTurnPreflightAuthority authority;
  return authority;
}

// --------------------------------------------------------
// Validate a restored committed pair before using it as a counter-exhaustion test seam.
model::Result<BookIdentity> BookIdentity::from_committed(model::BookGeneration generation,
                                                         model::BookRevision revision) {
  if (revision.value() < generation.value()) {
    return model::Result<BookIdentity>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidMarketEvent, "market_book.book_identity"));
  }
  return model::Result<BookIdentity>::create_success(BookIdentity{generation, revision});
}

// --------------------------------------------------------
// Advance both counters atomically in temporary values for a new authoritative snapshot.
model::Result<BookIdentity> BookIdentity::derive_next_snapshot_identity() const {
  auto generation = generation_.derive_next_ordinal();
  if (!generation) {
    return model::Result<BookIdentity>::create_failure(generation.error());
  }
  auto revision = revision_.derive_next_ordinal();
  if (!revision) {
    return model::Result<BookIdentity>::create_failure(revision.error());
  }
  return model::Result<BookIdentity>::create_success(
      BookIdentity{std::move(generation).value(), std::move(revision).value()});
}

// --------------------------------------------------------
// Advance only revision for an incremental commit while preserving snapshot generation.
model::Result<BookIdentity> BookIdentity::derive_next_delta_identity() const {
  auto revision = revision_.derive_next_ordinal();
  if (!revision) {
    return model::Result<BookIdentity>::create_failure(revision.error());
  }
  return model::Result<BookIdentity>::create_success(
      BookIdentity{generation_, std::move(revision).value()});
}

// --------------------------------------------------------
// M2 fixture integrity trusts only the normalized adapter verdict; candidate shape is validated by
// the independent fixed-book boundary and no venue-specific checksum is embedded here.
model::Result<void> validate_recorded_fixture_integrity(const NormalizedMarketUpdate& update,
                                                        ReadyBookView candidate) {
  static_cast<void>(candidate);
  if (update.integrity().verdict != IntegrityVerdict::Accepted) {
    return create_market_state_failure_result(model::DomainErrorCode::MarketIntegrityFailure,
                                              "market_update.integrity_verdict");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Validate immutable source-to-metadata attribution and all policy-owned owner-state bounds.
model::Result<MarketStateMachine> MarketStateMachine::create_market_state_machine(
    const runtime::RuntimePolicy& policy, const runtime::RuntimeSource& source,
    model::InstrumentMetadata metadata, MarketIntegrityValidator integrity_validator) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve the complete source value through the sealed policy before projecting any identity.
  const auto* configured_source = policy.find_source(source.definition().source_id);
  if (configured_source == nullptr || *configured_source != source) {
    return model::Result<MarketStateMachine>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::RuntimeSourceNotConfigured, "market_state.source"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The metadata copy must describe the exact configured source tuple and immutable revision.
  const auto& definition = source.definition();
  if (metadata.venue_id() != definition.venue_id ||
      metadata.instrument_id() != definition.instrument_id ||
      metadata.venue_instrument_id() != definition.venue_instrument_id ||
      metadata.revision() != definition.metadata_revision) {
    return model::Result<MarketStateMachine>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::MarketMetadataMismatch, "market_state.metadata"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The complete RuntimePolicy owns every validated bound; only the caller-supplied integrity seam
  // remains independently fallible at this factory boundary.
  if (integrity_validator == nullptr) {
    return model::Result<MarketStateMachine>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidRuntimePolicy, "market_state.runtime_policy"));
  }
  const auto& limits = policy.limits();

  // ++++++++++++++++++++++++++++++++++++++++
  // Both source projections, bounds, and expected trace provenance derive from one sealed policy.
  return model::Result<MarketStateMachine>::create_success(MarketStateMachine{
      MarketSourceIdentity::from_runtime_source(source),
      trace::RuntimeTraceSource::from_runtime_source(source), std::move(metadata),
      static_cast<std::size_t>(limits.retained_book_depth),
      static_cast<std::size_t>(limits.maximum_changes_per_update),
      limits.stale_threshold_nanoseconds, source.matching_subscription_count(),
      limits.maximum_callbacks_per_turn, trace::RuntimeTraceProvenance::from_runtime_policy(policy),
      integrity_validator});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Publish the initial explicit Synchronizing state only after its event and exact trace fan-out
// fit.
model::Result<MarketTurnOutcome>
MarketStateMachine::initialize_market_state(const OwnerMarketTurnContext& context,
                                            trace::RuntimeTraceSink& trace_sink,
                                            MarketTurnPreflightAuthority& preflight_authority) {
  auto provenance_valid = require_trace_provenance(expected_trace_provenance_, trace_sink);
  if (!provenance_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(provenance_valid.error());
  }
  if (readiness_) {
    return model::Result<MarketTurnOutcome>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidMarketState, "market_state.initialization"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove the sanitized transition and its independently shaped canonical record before mutation.
  auto event_fields = create_owner_state_event_fields(source_, context, std::nullopt,
                                                      MarketReadiness::Synchronizing, std::nullopt);
  auto event_validation = validate_market_state_transition(event_fields);
  if (!event_validation) {
    return propagate_market_state_failure<MarketTurnOutcome>(event_validation.error());
  }
  MarketTurnTraceDraftBatch drafts;
  drafts.append_trace_draft(
      trace::RuntimeTraceEventKind::MarketStateTransition,
      create_owner_transition_trace_fields(trace_source_, context, std::nullopt,
                                           MarketReadiness::Synchronizing, std::nullopt));
  auto fanout =
      calculate_fanout_budget(matching_subscription_count_, 1U, maximum_callbacks_per_turn_);
  if (!fanout) {
    return model::Result<MarketTurnOutcome>::create_failure(fanout.error());
  }
  auto trace_result = append_trace_drafts(drafts, source_.source_ordinal(), context.turn_ordinal,
                                          1U, matching_subscription_count_, fanout.value(),
                                          preflight_authority, trace_sink);
  if (!trace_result) {
    return propagate_market_state_failure<MarketTurnOutcome>(trace_result.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // After all fallible preconditions, publish state and construct the prevalidated event.
  readiness_ = MarketReadiness::Synchronizing;
  auto state_event = MarketStateEvent{std::move(event_fields)};
  return model::Result<MarketTurnOutcome>::create_success(
      MarketTurnOutcome{source_.source_ordinal(), context.turn_ordinal, *readiness_, std::nullopt,
                        std::move(state_event), std::nullopt, std::nullopt, false,
                        fanout.value().callback_count, fanout.value().callback_trace_records});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Explicit resynchronization clears continuity and hides retained book bytes until a fresh
// snapshot.
model::Result<MarketTurnOutcome>
MarketStateMachine::resynchronize_source(const OwnerMarketTurnContext& context,
                                         trace::RuntimeTraceSink& trace_sink,
                                         MarketTurnPreflightAuthority& preflight_authority) {
  auto provenance_valid = require_trace_provenance(expected_trace_provenance_, trace_sink);
  if (!provenance_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(provenance_valid.error());
  }
  auto initialized = require_initialized(readiness_);
  if (!initialized) {
    return propagate_market_state_failure<MarketTurnOutcome>(initialized.error());
  }
  const auto previous = *readiness_;

  // ++++++++++++++++++++++++++++++++++++++++
  // Explicit recovery intent is observable even when the owner was already Synchronizing.
  MarketTurnTraceDraftBatch drafts;
  auto event_fields = create_owner_state_event_fields(
      source_, context, previous, MarketReadiness::Synchronizing, book_identity_);
  auto validation = validate_market_state_transition(event_fields);
  if (!validation) {
    return propagate_market_state_failure<MarketTurnOutcome>(validation.error());
  }
  drafts.append_trace_draft(trace::RuntimeTraceEventKind::MarketStateTransition,
                            create_owner_transition_trace_fields(trace_source_, context, previous,
                                                                 MarketReadiness::Synchronizing,
                                                                 book_identity_));
  auto fanout =
      calculate_fanout_budget(matching_subscription_count_, 1U, maximum_callbacks_per_turn_);
  if (!fanout) {
    return model::Result<MarketTurnOutcome>::create_failure(fanout.error());
  }
  auto trace_result = append_trace_drafts(drafts, source_.source_ordinal(), context.turn_ordinal,
                                          1U, matching_subscription_count_, fanout.value(),
                                          preflight_authority, trace_sink);
  if (!trace_result) {
    return propagate_market_state_failure<MarketTurnOutcome>(trace_result.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Commit the continuity reset only after exact evidence feasibility is proven.
  readiness_ = MarketReadiness::Synchronizing;
  active_session_.reset();
  last_source_sequence_.reset();
  last_payload_digest_.reset();
  last_commit_time_.reset();
  auto state_event = MarketStateEvent{std::move(event_fields)};
  return model::Result<MarketTurnOutcome>::create_success(
      MarketTurnOutcome{source_.source_ordinal(), context.turn_ordinal, *readiness_, std::nullopt,
                        std::move(state_event), std::nullopt, std::nullopt, false,
                        fanout.value().callback_count, fanout.value().callback_trace_records});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Classify one normalized update in fixed policy order and swap scratch only after every check.
model::Result<MarketTurnOutcome> MarketStateMachine::apply_market_update(
    NormalizedMarketUpdate update, const AcceptedMarketTurnContext& context,
    trace::RuntimeTraceSink& trace_sink, MarketTurnPreflightAuthority& preflight_authority) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject owner-contract defects before classifying source-authored market semantics.
  auto provenance_valid = require_trace_provenance(expected_trace_provenance_, trace_sink);
  if (!provenance_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(provenance_valid.error());
  }
  auto initialized = require_initialized(readiness_);
  if (!initialized) {
    return propagate_market_state_failure<MarketTurnOutcome>(initialized.error());
  }
  auto source_valid = require_source(source_, update.source());
  if (!source_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(source_valid.error());
  }
  if (update.changes().size() > maximum_changes_per_update_) {
    return model::Result<MarketTurnOutcome>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::MarketBookCapacityExceeded, "market_update.changes"));
  }
  auto time_valid =
      require_processing_order(context.processing_timestamp, update.receive_timestamp());
  if (!time_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(time_valid.error());
  }
  const auto previous = *readiness_;

  // ++++++++++++++++++++++++++++++++++++++++
  // Semantic rejection/ignore outcomes still append one input disposition and an optional state
  // transition, but never touch the current book or its counters.
  const auto finish_without_book = [&](trace::RuntimeInputDisposition disposition,
                                       MarketReadiness next,
                                       bool adopt_new_session) -> model::Result<MarketTurnOutcome> {
    const bool state_changes = next != previous;
    MarketTurnTraceDraftBatch drafts;
    drafts.append_trace_draft(trace::RuntimeTraceEventKind::InputDisposition,
                              create_update_input_trace_fields(trace_source_, update, context,
                                                               disposition, next, book_identity_));

    std::optional<MarketStateEventFields> event_fields;
    if (state_changes) {
      // A newer-session non-snapshot intentionally sanitizes its transition to trusted envelope
      // context, while the separate disposition record retains the full parsed-update identity.
      if (next == MarketReadiness::Synchronizing) {
        event_fields = create_envelope_state_event_fields(
            source_, update.session_epoch(), update.receive_sequence(), update.receive_timestamp(),
            context, previous, next, book_identity_);
        drafts.append_trace_draft(
            trace::RuntimeTraceEventKind::MarketStateTransition,
            create_envelope_transition_trace_fields(trace_source_, update.session_epoch(),
                                                    update.receive_sequence(), context, previous,
                                                    next, book_identity_));
      } else {
        event_fields = create_update_state_event_fields(source_, update, context, previous, next,
                                                        book_identity_);
        drafts.append_trace_draft(trace::RuntimeTraceEventKind::MarketStateTransition,
                                  create_update_transition_trace_fields(trace_source_, update,
                                                                        context, previous, next,
                                                                        book_identity_));
      }
      auto validation = validate_market_state_transition(*event_fields);
      if (!validation) {
        return propagate_market_state_failure<MarketTurnOutcome>(validation.error());
      }
    }

    const std::uint32_t event_count = state_changes ? 1U : 0U;
    auto fanout = calculate_fanout_budget(matching_subscription_count_, event_count,
                                          maximum_callbacks_per_turn_);
    if (!fanout) {
      return model::Result<MarketTurnOutcome>::create_failure(fanout.error());
    }
    auto trace_result = append_trace_drafts(drafts, source_.source_ordinal(), context.turn_ordinal,
                                            event_count, matching_subscription_count_,
                                            fanout.value(), preflight_authority, trace_sink);
    if (!trace_result) {
      return propagate_market_state_failure<MarketTurnOutcome>(trace_result.error());
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Commit only the classified readiness/session consequence after trace feasibility is proven.
    readiness_ = next;
    if (adopt_new_session) {
      active_session_ = update.session_epoch();
      last_source_sequence_.reset();
      last_payload_digest_.reset();
      last_commit_time_.reset();
    }
    std::optional<MarketStateEvent> state_event;
    if (event_fields) {
      state_event = MarketStateEvent{std::move(*event_fields)};
    }
    return model::Result<MarketTurnOutcome>::create_success(
        MarketTurnOutcome{source_.source_ordinal(), context.turn_ordinal, *readiness_, disposition,
                          std::move(state_event), std::nullopt, std::nullopt, false,
                          fanout.value().callback_count, fanout.value().callback_trace_records});
  };

  // ++++++++++++++++++++++++++++++++++++++++
  // Session age and same-session sequence/digest identity precede metadata or integrity checks.
  const bool has_session = active_session_.has_value();
  if (has_session && update.session_epoch() < *active_session_) {
    return finish_without_book(trace::RuntimeInputDisposition::OlderInputIgnored, previous, false);
  }
  const bool newer_session = !has_session || update.session_epoch() > *active_session_;
  if (!newer_session && last_source_sequence_) {
    if (update.source_sequence() < *last_source_sequence_) {
      return finish_without_book(trace::RuntimeInputDisposition::OlderInputIgnored, previous,
                                 false);
    }
    if (update.source_sequence() == *last_source_sequence_) {
      const bool exact_duplicate =
          last_payload_digest_ && update.payload_digest() == *last_payload_digest_;
      return finish_without_book(exact_duplicate
                                     ? trace::RuntimeInputDisposition::ExactDuplicateIgnored
                                     : trace::RuntimeInputDisposition::SequenceConflictRejected,
                                 exact_duplicate ? previous : MarketReadiness::Invalid, false);
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Only a snapshot may establish a newer session or recover any non-Ready state.
  if (newer_session && update.kind() != MarketUpdateKind::Snapshot) {
    return finish_without_book(trace::RuntimeInputDisposition::NonReadyDeltaRejected,
                               MarketReadiness::Synchronizing, true);
  }
  if (update.kind() == MarketUpdateKind::Delta && previous != MarketReadiness::Ready) {
    return finish_without_book(trace::RuntimeInputDisposition::NonReadyDeltaRejected, previous,
                               false);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Ready deltas require the exact explicit predecessor; snapshots deliberately start a new anchor.
  if (update.kind() == MarketUpdateKind::Delta &&
      (!last_source_sequence_ || !update.predecessor_sequence() ||
       *update.predecessor_sequence() != *last_source_sequence_)) {
    return finish_without_book(trace::RuntimeInputDisposition::GapRejected,
                               MarketReadiness::Invalid, false);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Immutable metadata and fixture integrity have deterministic precedence over book structure.
  if (update.metadata_revision() != metadata_.revision()) {
    return finish_without_book(trace::RuntimeInputDisposition::MetadataRevisionRejected,
                               MarketReadiness::Invalid, false);
  }
  if (update.integrity().verdict != IntegrityVerdict::Accepted) {
    return finish_without_book(trace::RuntimeInputDisposition::ChecksumRejected,
                               MarketReadiness::Invalid, false);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Build the complete candidate in stable preallocated scratch storage, starting empty for an
  // authoritative snapshot and from current for an absolute-quantity delta.
  if (update.kind() == MarketUpdateKind::Snapshot) {
    scratch_book_.clear_levels();
  } else {
    scratch_book_ = current_book_;
  }
  if (update.kind() == MarketUpdateKind::Delta) {
    for (const auto& change : update.changes()) {
      if (change.quantity.coefficient() == 0) {
        auto applied = scratch_book_.apply_level_change(change.side, change.price, change.quantity,
                                                        retained_depth_);
        if (!applied) {
          return finish_without_book(trace::RuntimeInputDisposition::StructuralBookRejected,
                                     MarketReadiness::Invalid, false);
        }
      }
    }
  }
  for (const auto& change : update.changes()) {
    if (update.kind() == MarketUpdateKind::Snapshot || change.quantity.coefficient() != 0) {
      auto applied = scratch_book_.apply_level_change(change.side, change.price, change.quantity,
                                                      retained_depth_);
      if (!applied) {
        return finish_without_book(trace::RuntimeInputDisposition::StructuralBookRejected,
                                   MarketReadiness::Invalid, false);
      }
    }
  }
  auto candidate_valid = scratch_book_.validate_book_state(metadata_, retained_depth_);
  if (!candidate_valid) {
    return finish_without_book(trace::RuntimeInputDisposition::StructuralBookRejected,
                               MarketReadiness::Invalid, false);
  }
  auto integrity_valid = integrity_validator_(update, scratch_book_.ready_view());
  if (!integrity_valid) {
    return finish_without_book(trace::RuntimeInputDisposition::ChecksumRejected,
                               MarketReadiness::Invalid, false);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Advance book counters in temporary state so exhaustion cannot partially publish a candidate.
  model::Result<BookIdentity> next_identity =
      update.kind() == MarketUpdateKind::Snapshot
          ? (book_identity_
                 ? book_identity_->derive_next_snapshot_identity()
                 : model::Result<BookIdentity>::create_success(BookIdentity::create_initial()))
          : book_identity_->derive_next_delta_identity();
  if (!next_identity) {
    return model::Result<MarketTurnOutcome>::create_failure(next_identity.error());
  }
  const auto committed_identity = next_identity.value();
  const bool state_changes = previous != MarketReadiness::Ready;

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate exact input/state evidence and reserve callbacks plus first-reentry records before
  // the no-fail swap. Recovery snapshots dispatch state before market data.
  MarketTurnTraceDraftBatch drafts;
  const auto disposition = update.kind() == MarketUpdateKind::Snapshot
                               ? trace::RuntimeInputDisposition::SnapshotApplied
                               : trace::RuntimeInputDisposition::DeltaApplied;
  drafts.append_trace_draft(trace::RuntimeTraceEventKind::InputDisposition,
                            create_update_input_trace_fields(trace_source_, update, context,
                                                             disposition, MarketReadiness::Ready,
                                                             committed_identity));
  std::optional<MarketStateEventFields> state_fields;
  if (state_changes) {
    state_fields = create_update_state_event_fields(source_, update, context, previous,
                                                    MarketReadiness::Ready, committed_identity);
    auto validation = validate_market_state_transition(*state_fields);
    if (!validation) {
      return propagate_market_state_failure<MarketTurnOutcome>(validation.error());
    }
    drafts.append_trace_draft(
        trace::RuntimeTraceEventKind::MarketStateTransition,
        create_update_transition_trace_fields(trace_source_, update, context, previous,
                                              MarketReadiness::Ready, committed_identity));
  }
  const std::uint32_t event_count = state_changes ? 2U : 1U;
  auto fanout = calculate_fanout_budget(matching_subscription_count_, event_count,
                                        maximum_callbacks_per_turn_);
  if (!fanout) {
    return model::Result<MarketTurnOutcome>::create_failure(fanout.error());
  }
  auto trace_result = append_trace_drafts(drafts, source_.source_ordinal(), context.turn_ordinal,
                                          event_count, matching_subscription_count_, fanout.value(),
                                          preflight_authority, trace_sink);
  if (!trace_result) {
    return propagate_market_state_failure<MarketTurnOutcome>(trace_result.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Swap and publish continuity atomically after every fallible candidate, event, and trace check.
  const auto committed_digest = update.payload_digest();
  const auto committed_session = update.session_epoch();
  const auto committed_sequence = update.source_sequence();
  scratch_book_.swap(current_book_);
  book_identity_ = committed_identity;
  readiness_ = MarketReadiness::Ready;
  active_session_ = committed_session;
  last_source_sequence_ = committed_sequence;
  last_payload_digest_ = committed_digest;
  last_commit_time_ = context.processing_timestamp;

  // ++++++++++++++++++++++++++++++++++++++++
  // Construct only prevalidated event values after the complete commit is owner-visible.
  std::optional<MarketStateEvent> state_event;
  if (state_fields) {
    state_event = MarketStateEvent{std::move(*state_fields)};
  }
  const MarketCommitContext commit_context{
      context.admission_ordinal, context.turn_ordinal, context.processing_timestamp,
      committed_identity.generation(), committed_identity.revision()};
  auto market_event = MarketEvent{std::move(update), commit_context};
  auto view = current_book_.ready_view();
  return model::Result<MarketTurnOutcome>::create_success(
      MarketTurnOutcome{source_.source_ordinal(), context.turn_ordinal, *readiness_, disposition,
                        std::move(state_event), std::move(market_event), std::move(view), true,
                        fanout.value().callback_count, fanout.value().callback_trace_records});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Advance only to a strictly newer explicit session and clear its prior continuity anchor.
model::Result<MarketTurnOutcome> MarketStateMachine::apply_session_start(
    const SessionStarted& control, const AcceptedMarketTurnContext& context,
    trace::RuntimeTraceSink& trace_sink, MarketTurnPreflightAuthority& preflight_authority) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate trusted source and owner timing before applying session-age policy.
  auto provenance_valid = require_trace_provenance(expected_trace_provenance_, trace_sink);
  if (!provenance_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(provenance_valid.error());
  }
  auto initialized = require_initialized(readiness_);
  if (!initialized) {
    return propagate_market_state_failure<MarketTurnOutcome>(initialized.error());
  }
  auto source_valid = require_source(source_, control.source);
  if (!source_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(source_valid.error());
  }
  auto time_valid =
      require_processing_order(context.processing_timestamp, control.receive_timestamp);
  if (!time_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(time_valid.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Same and older controls are observable ignores; only a newer epoch resets continuity.
  const auto previous = *readiness_;
  const bool newer = !active_session_ || control.session_epoch > *active_session_;
  const auto disposition = newer ? trace::RuntimeInputDisposition::SessionReset
                                 : trace::RuntimeInputDisposition::SessionIgnored;
  const auto next = newer ? MarketReadiness::Synchronizing : previous;
  const bool state_changes = next != previous;
  MarketTurnTraceDraftBatch drafts;
  drafts.append_trace_draft(trace::RuntimeTraceEventKind::InputDisposition,
                            create_envelope_input_trace_fields(trace_source_, control.session_epoch,
                                                               control.receive_sequence, context,
                                                               disposition, next, book_identity_));

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish a sanitized state event only when the newer session actually changes readiness.
  std::optional<MarketStateEventFields> event_fields;
  if (state_changes) {
    event_fields = create_envelope_state_event_fields(
        source_, control.session_epoch, control.receive_sequence, control.receive_timestamp,
        context, previous, next, book_identity_);
    auto validation = validate_market_state_transition(*event_fields);
    if (!validation) {
      return propagate_market_state_failure<MarketTurnOutcome>(validation.error());
    }
    drafts.append_trace_draft(trace::RuntimeTraceEventKind::MarketStateTransition,
                              create_envelope_transition_trace_fields(
                                  trace_source_, control.session_epoch, control.receive_sequence,
                                  context, previous, next, book_identity_));
  }
  const std::uint32_t event_count = state_changes ? 1U : 0U;
  auto fanout = calculate_fanout_budget(matching_subscription_count_, event_count,
                                        maximum_callbacks_per_turn_);
  if (!fanout) {
    return model::Result<MarketTurnOutcome>::create_failure(fanout.error());
  }
  auto trace_result = append_trace_drafts(drafts, source_.source_ordinal(), context.turn_ordinal,
                                          event_count, matching_subscription_count_, fanout.value(),
                                          preflight_authority, trace_sink);
  if (!trace_result) {
    return propagate_market_state_failure<MarketTurnOutcome>(trace_result.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reset continuity only after the exact evidence prefix is accepted.
  if (newer) {
    readiness_ = next;
    active_session_ = control.session_epoch;
    last_source_sequence_.reset();
    last_payload_digest_.reset();
    last_commit_time_.reset();
  }
  std::optional<MarketStateEvent> state_event;
  if (event_fields) {
    state_event = MarketStateEvent{std::move(*event_fields)};
  }
  return model::Result<MarketTurnOutcome>::create_success(
      MarketTurnOutcome{source_.source_ordinal(), context.turn_ordinal, *readiness_, disposition,
                        std::move(state_event), std::nullopt, std::nullopt, false,
                        fanout.value().callback_count, fanout.value().callback_trace_records});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Evaluate freshness only on this explicit accepted turn and transition Ready at the exact
// deadline.
model::Result<MarketTurnOutcome> MarketStateMachine::apply_staleness_check(
    const StalenessCheck& control, const AcceptedMarketTurnContext& context,
    trace::RuntimeTraceSink& trace_sink, MarketTurnPreflightAuthority& preflight_authority) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Bind the recorded check time to the owner's turn and validate its accepted envelope.
  auto provenance_valid = require_trace_provenance(expected_trace_provenance_, trace_sink);
  if (!provenance_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(provenance_valid.error());
  }
  auto initialized = require_initialized(readiness_);
  if (!initialized) {
    return propagate_market_state_failure<MarketTurnOutcome>(initialized.error());
  }
  auto source_valid = require_source(source_, control.source);
  if (!source_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(source_valid.error());
  }
  if (control.processing_timestamp != context.processing_timestamp) {
    return model::Result<MarketTurnOutcome>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidMarketEvent, "staleness_check.processing_timestamp"));
  }
  auto time_valid =
      require_processing_order(context.processing_timestamp, control.receive_timestamp);
  if (!time_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(time_valid.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Checks outside the active session are observable ignores and never refresh or invalidate state.
  const auto previous = *readiness_;
  const bool active = active_session_ && control.session_epoch == *active_session_;
  auto disposition = active ? trace::RuntimeInputDisposition::StalenessChecked
                            : trace::RuntimeInputDisposition::SessionIgnored;
  auto next = previous;
  if (active && previous == MarketReadiness::Ready) {
    if (!last_commit_time_) {
      return model::Result<MarketTurnOutcome>::create_failure(model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidMarketState, "market_state.last_commit_time"));
    }
    if (context.processing_timestamp < *last_commit_time_) {
      return model::Result<MarketTurnOutcome>::create_failure(model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidTimestampOrder, "staleness_check.processing_timestamp"));
    }
    const auto elapsed =
        context.processing_timestamp.nanoseconds() - last_commit_time_->nanoseconds();
    if (elapsed >= stale_threshold_nanoseconds_) {
      next = MarketReadiness::Stale;
    }
  }
  const bool state_changes = next != previous;

  // ++++++++++++++++++++++++++++++++++++++++
  // Prevalidate the envelope disposition and optional Ready-to-Stale transition before mutation.
  MarketTurnTraceDraftBatch drafts;
  drafts.append_trace_draft(trace::RuntimeTraceEventKind::InputDisposition,
                            create_envelope_input_trace_fields(trace_source_, control.session_epoch,
                                                               control.receive_sequence, context,
                                                               disposition, next, book_identity_));
  std::optional<MarketStateEventFields> event_fields;
  if (state_changes) {
    event_fields = create_envelope_state_event_fields(
        source_, control.session_epoch, control.receive_sequence, control.receive_timestamp,
        context, previous, next, book_identity_);
    auto validation = validate_market_state_transition(*event_fields);
    if (!validation) {
      return propagate_market_state_failure<MarketTurnOutcome>(validation.error());
    }
    drafts.append_trace_draft(trace::RuntimeTraceEventKind::MarketStateTransition,
                              create_envelope_transition_trace_fields(
                                  trace_source_, control.session_epoch, control.receive_sequence,
                                  context, previous, next, book_identity_));
  }
  const std::uint32_t event_count = state_changes ? 1U : 0U;
  auto fanout = calculate_fanout_budget(matching_subscription_count_, event_count,
                                        maximum_callbacks_per_turn_);
  if (!fanout) {
    return model::Result<MarketTurnOutcome>::create_failure(fanout.error());
  }
  auto trace_result = append_trace_drafts(drafts, source_.source_ordinal(), context.turn_ordinal,
                                          event_count, matching_subscription_count_, fanout.value(),
                                          preflight_authority, trace_sink);
  if (!trace_result) {
    return propagate_market_state_failure<MarketTurnOutcome>(trace_result.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Only the explicit at-or-beyond threshold check changes state; it never mutates the book.
  readiness_ = next;
  std::optional<MarketStateEvent> state_event;
  if (event_fields) {
    state_event = MarketStateEvent{std::move(*event_fields)};
  }
  return model::Result<MarketTurnOutcome>::create_success(
      MarketTurnOutcome{source_.source_ordinal(), context.turn_ordinal, *readiness_, disposition,
                        std::move(state_event), std::nullopt, std::nullopt, false,
                        fanout.value().callback_count, fanout.value().callback_trace_records});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Convert attributable malformed/unsupported input into only a sanitized deterministic transition.
model::Result<MarketTurnOutcome> MarketStateMachine::apply_attributable_failure(
    const AttributableMarketFailure& rejected, const AcceptedMarketTurnContext& context,
    trace::RuntimeTraceSink& trace_sink, MarketTurnPreflightAuthority& preflight_authority) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Only assigned sanitized parser dispositions may enter this containment boundary.
  auto provenance_valid = require_trace_provenance(expected_trace_provenance_, trace_sink);
  if (!provenance_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(provenance_valid.error());
  }
  auto initialized = require_initialized(readiness_);
  if (!initialized) {
    return propagate_market_state_failure<MarketTurnOutcome>(initialized.error());
  }
  auto source_valid = require_source(source_, rejected.source);
  if (!source_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(source_valid.error());
  }
  if (rejected.disposition != trace::RuntimeInputDisposition::MalformedRejected &&
      rejected.disposition != trace::RuntimeInputDisposition::UnsupportedRejected) {
    return model::Result<MarketTurnOutcome>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidMarketEvent, "market_failure.disposition"));
  }
  auto time_valid =
      require_processing_order(context.processing_timestamp, rejected.receive_timestamp);
  if (!time_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(time_valid.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Session-age classification precedes malformed-content containment just as it does for updates.
  const auto previous = *readiness_;
  const bool older_session = active_session_ && rejected.session_epoch < *active_session_;
  const auto disposition =
      older_session ? trace::RuntimeInputDisposition::SessionIgnored : rejected.disposition;
  const auto next = older_session ? previous : MarketReadiness::Invalid;
  const bool state_changes = next != previous;
  MarketTurnTraceDraftBatch drafts;
  drafts.append_trace_draft(trace::RuntimeTraceEventKind::InputDisposition,
                            create_envelope_input_trace_fields(
                                trace_source_, rejected.session_epoch, rejected.receive_sequence,
                                context, disposition, next, book_identity_));

  // ++++++++++++++++++++++++++++++++++++++++
  // Strategies see only a sanitized envelope transition, never malformed frame bytes or offsets.
  std::optional<MarketStateEventFields> event_fields;
  if (state_changes) {
    event_fields = create_envelope_state_event_fields(
        source_, rejected.session_epoch, rejected.receive_sequence, rejected.receive_timestamp,
        context, previous, next, book_identity_);
    auto validation = validate_market_state_transition(*event_fields);
    if (!validation) {
      return propagate_market_state_failure<MarketTurnOutcome>(validation.error());
    }
    drafts.append_trace_draft(trace::RuntimeTraceEventKind::MarketStateTransition,
                              create_envelope_transition_trace_fields(
                                  trace_source_, rejected.session_epoch, rejected.receive_sequence,
                                  context, previous, next, book_identity_));
  }
  const std::uint32_t event_count = state_changes ? 1U : 0U;
  auto fanout = calculate_fanout_budget(matching_subscription_count_, event_count,
                                        maximum_callbacks_per_turn_);
  if (!fanout) {
    return model::Result<MarketTurnOutcome>::create_failure(fanout.error());
  }
  auto trace_result = append_trace_drafts(drafts, source_.source_ordinal(), context.turn_ordinal,
                                          event_count, matching_subscription_count_, fanout.value(),
                                          preflight_authority, trace_sink);
  if (!trace_result) {
    return propagate_market_state_failure<MarketTurnOutcome>(trace_result.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Containment changes only readiness; prior book and continuity anchors remain available solely
  // for deterministic recovery classification.
  readiness_ = next;
  std::optional<MarketStateEvent> state_event;
  if (event_fields) {
    state_event = MarketStateEvent{std::move(*event_fields)};
  }
  return model::Result<MarketTurnOutcome>::create_success(
      MarketTurnOutcome{source_.source_ordinal(), context.turn_ordinal, *readiness_, disposition,
                        std::move(state_event), std::nullopt, std::nullopt, false,
                        fanout.value().callback_count, fanout.value().callback_trace_records});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Consume one ordered source-loss fence and fail closed without fabricating receive identity.
model::Result<MarketTurnOutcome> MarketStateMachine::apply_source_discontinuity(
    model::AdmissionOrdinal failed_admission, const OwnerMarketTurnContext& context,
    trace::RuntimeTraceSink& trace_sink, MarketTurnPreflightAuthority& preflight_authority) {
  auto provenance_valid = require_trace_provenance(expected_trace_provenance_, trace_sink);
  if (!provenance_valid) {
    return propagate_market_state_failure<MarketTurnOutcome>(provenance_valid.error());
  }
  auto initialized = require_initialized(readiness_);
  if (!initialized) {
    return propagate_market_state_failure<MarketTurnOutcome>(initialized.error());
  }
  const auto previous = *readiness_;
  const auto next = MarketReadiness::Invalid;
  const bool state_changes = previous != next;

  // ++++++++++++++++++++++++++++++++++++++++
  // The fence disposition always records loss; a transition/event appears only on the first change.
  MarketTurnTraceDraftBatch drafts;
  drafts.append_trace_draft(trace::RuntimeTraceEventKind::InputDisposition,
                            create_discontinuity_input_trace_fields(trace_source_, failed_admission,
                                                                    context.turn_ordinal, next,
                                                                    book_identity_));
  std::optional<MarketStateEventFields> event_fields;
  if (state_changes) {
    event_fields = create_discontinuity_state_event_fields(source_, failed_admission, context,
                                                           previous, book_identity_);
    auto validation = validate_market_state_transition(*event_fields);
    if (!validation) {
      return propagate_market_state_failure<MarketTurnOutcome>(validation.error());
    }
    drafts.append_trace_draft(
        trace::RuntimeTraceEventKind::MarketStateTransition,
        create_discontinuity_transition_trace_fields(
            trace_source_, failed_admission, context.turn_ordinal, previous, book_identity_));
  }
  const std::uint32_t event_count = state_changes ? 1U : 0U;
  auto fanout = calculate_fanout_budget(matching_subscription_count_, event_count,
                                        maximum_callbacks_per_turn_);
  if (!fanout) {
    return model::Result<MarketTurnOutcome>::create_failure(fanout.error());
  }
  auto trace_result = append_trace_drafts(drafts, source_.source_ordinal(), context.turn_ordinal,
                                          event_count, matching_subscription_count_, fanout.value(),
                                          preflight_authority, trace_sink);
  if (!trace_result) {
    return propagate_market_state_failure<MarketTurnOutcome>(trace_result.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve the old book and continuity anchor while hiding both behind Invalid until a snapshot.
  readiness_ = next;
  std::optional<MarketStateEvent> state_event;
  if (event_fields) {
    state_event = MarketStateEvent{std::move(*event_fields)};
  }
  return model::Result<MarketTurnOutcome>::create_success(MarketTurnOutcome{
      source_.source_ordinal(), context.turn_ordinal, *readiness_,
      trace::RuntimeInputDisposition::SourceDiscontinuity, std::move(state_event), std::nullopt,
      std::nullopt, false, fanout.value().callback_count, fanout.value().callback_trace_records});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Expose the current immutable prefixes only while explicit readiness authorizes market callbacks.
model::Result<ReadyBookView> MarketStateMachine::create_ready_book_view() const {
  if (readiness_ != MarketReadiness::Ready || !book_identity_) {
    return model::Result<ReadyBookView>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::MarketNotReady, "market_state.ready_book"));
  }
  return model::Result<ReadyBookView>::create_success(current_book_.ready_view());
}

// --------------------------------------------------------

} // namespace aegis::market_data
