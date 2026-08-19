// Purpose: validate bot-strategy ownership and perform synchronous canonical subscription dispatch
// with callback trace, re-entry, and duration evidence.

#include "aegis/runtime/bot_runtime.hpp"

#include "aegis/model/domain_error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::runtime {
namespace {

using model::DomainError;
using model::DomainErrorCode;

// --------------------------------------------------------
// Build one stable field-key failure without repeating Result conversion at each guard.
template <typename Value>
[[nodiscard]] model::Result<Value> failure(DomainErrorCode code, std::string_view field) {
  return model::Result<Value>::failure(DomainError::at_field(code, std::string{field}));
}

// --------------------------------------------------------
// Convert strategy-visible readiness into the independent canonical trace vocabulary.
[[nodiscard]] trace::RuntimeMarketState runtime_state(market_data::MarketReadiness readiness) {
  switch (readiness) {
  case market_data::MarketReadiness::Synchronizing:
    return trace::RuntimeMarketState::Synchronizing;
  case market_data::MarketReadiness::Ready:
    return trace::RuntimeMarketState::Ready;
  case market_data::MarketReadiness::Stale:
    return trace::RuntimeMarketState::Stale;
  case market_data::MarketReadiness::Invalid:
    return trace::RuntimeMarketState::Invalid;
  default:
    return trace::RuntimeMarketState::Unspecified;
  }
}

// --------------------------------------------------------
// Own the complete canonical trace shape for one sanitized state callback.
template <typename Grant>
[[nodiscard]] trace::RuntimeTraceFields
state_callback_fields(const Grant& grant, const market_data::MarketStateEvent& event,
                      model::CallbackOrdinal callback_ordinal) {
  const auto& source = event.fields();
  trace::RuntimeTraceFields fields;
  fields.source = grant.trace_source;
  fields.bot_id = grant.attribution.bot_id;
  fields.subscription_id = grant.subscription.id;
  fields.admission_ordinal = source.admission_ordinal;
  fields.turn_ordinal = source.turn_ordinal;
  fields.callback_ordinal = callback_ordinal;
  fields.session_epoch = source.session_epoch;
  fields.source_sequence = source.source_sequence;
  fields.receive_sequence = source.receive_sequence;
  fields.metadata_revision = source.metadata_revision;
  fields.book_generation = source.book_generation;
  fields.book_revision = source.book_revision;
  fields.previous_state = source.previous_readiness
                              ? runtime_state(source.previous_readiness.value())
                              : trace::RuntimeMarketState::Unspecified;
  fields.state = runtime_state(source.readiness);
  return fields;
}

// --------------------------------------------------------
// Own the complete canonical trace shape for one committed Ready market callback.
template <typename Grant>
[[nodiscard]] trace::RuntimeTraceFields
market_callback_fields(const Grant& grant, const market_data::MarketEvent& event,
                       const market_data::ReadyBookView& book,
                       model::CallbackOrdinal callback_ordinal) {
  const auto& update = event.update();
  const auto& context = event.context();
  trace::RuntimeTraceFields fields;
  fields.source = grant.trace_source;
  fields.bot_id = grant.attribution.bot_id;
  fields.subscription_id = grant.subscription.id;
  fields.admission_ordinal = context.admission_ordinal;
  fields.turn_ordinal = context.turn_ordinal;
  fields.callback_ordinal = callback_ordinal;
  fields.session_epoch = update.session_epoch();
  fields.source_sequence = update.source_sequence();
  fields.receive_sequence = update.receive_sequence();
  fields.metadata_revision = update.metadata_revision();
  fields.book_generation = context.book_generation;
  fields.book_revision = context.book_revision;
  fields.state = trace::RuntimeMarketState::Ready;
  fields.best_bid = book.best_bid();
  fields.best_ask = book.best_ask();
  return fields;
}

// --------------------------------------------------------
// Resolve one source only when all present event forms agree exactly.
[[nodiscard]] std::optional<model::MarketSourceOrdinal>
outcome_source(const market_data::MarketTurnOutcome& outcome) noexcept {
  std::optional<model::MarketSourceOrdinal> source;
  if (outcome.state_event()) {
    source = outcome.state_event()->fields().source.source_ordinal();
  }
  if (outcome.market_event()) {
    const auto market_source = outcome.market_event()->update().source().source_ordinal();
    if (source && source.value() != market_source) {
      return std::nullopt;
    }
    source = market_source;
  }
  return source;
}

// --------------------------------------------------------
// Resolve one owner turn only when all present event forms agree exactly.
[[nodiscard]] std::optional<model::TurnOrdinal>
outcome_turn(const market_data::MarketTurnOutcome& outcome) noexcept {
  std::optional<model::TurnOrdinal> turn;
  if (outcome.state_event()) {
    turn = outcome.state_event()->fields().turn_ordinal;
  }
  if (outcome.market_event()) {
    const auto market_turn = outcome.market_event()->context().turn_ordinal;
    if (turn && turn.value() != market_turn) {
      return std::nullopt;
    }
    turn = market_turn;
  }
  return turn;
}

// --------------------------------------------------------
// Prebuild the identity-bearing portion shared by both assigned re-entry failure reasons.
template <typename Grant>
[[nodiscard]] trace::RuntimeTraceFields reentry_fields(const Grant& grant,
                                                       model::TurnOrdinal turn_ordinal,
                                                       model::CallbackOrdinal callback_ordinal) {
  trace::RuntimeTraceFields fields;
  fields.bot_id = grant.attribution.bot_id;
  fields.subscription_id = grant.subscription.id;
  fields.turn_ordinal = turn_ordinal;
  fields.callback_ordinal = callback_ordinal;
  return fields;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Validate provenance and exact strategy coverage, then preallocate canonical grant routing.
model::Result<BotRuntime>
BotRuntime::create(const configuration::StartupConfiguration& configuration,
                   const RuntimePolicy& policy, model::ClockProvider& measurement_clock,
                   trace::RuntimeTraceSink& trace_sink, RuntimeDiagnosticSink& diagnostics,
                   std::vector<BotStrategyRegistration> registrations,
                   BotRuntimeCounterSeed counter_seed) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Configuration, policy, and trace must all describe one immutable replay identity.
  if (policy.configuration_fingerprint() != configuration.fingerprint() ||
      trace_sink.provenance().configuration_fingerprint() != configuration.fingerprint() ||
      trace_sink.provenance().runtime_policy_fingerprint() != policy.fingerprint() ||
      diagnostics.configuration_fingerprint() != configuration.fingerprint() ||
      diagnostics.runtime_policy_fingerprint() != policy.fingerprint()) {
    return failure<BotRuntime>(DomainErrorCode::InvalidRelationship, "bot_runtime.provenance");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical bot-ID order fixes duplicate positions and makes exact configured coverage cheap.
  std::sort(registrations.begin(), registrations.end(),
            [](const BotStrategyRegistration& lhs, const BotStrategyRegistration& rhs) {
              return lhs.bot_id < rhs.bot_id;
            });
  for (std::size_t index = 0U; index < registrations.size(); ++index) {
    if (!registrations[index].strategy) {
      return failure<BotRuntime>(DomainErrorCode::StrategyNotConfigured,
                                 "bot_runtime.registrations.strategy");
    }
    if (index != 0U && registrations[index - 1U].bot_id == registrations[index].bot_id) {
      return failure<BotRuntime>(DomainErrorCode::StrategyNotConfigured,
                                 "bot_runtime.registrations.bot_id");
    }
  }

  const auto& attributions = configuration.organization().bot_attributions();
  if (registrations.size() != attributions.size()) {
    return failure<BotRuntime>(DomainErrorCode::StrategyNotConfigured,
                               "bot_runtime.registrations.coverage");
  }
  for (std::size_t index = 0U; index < attributions.size(); ++index) {
    if (registrations[index].bot_id != attributions[index].bot_id) {
      return failure<BotRuntime>(DomainErrorCode::StrategyNotConfigured,
                                 "bot_runtime.registrations.coverage");
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Transfer one strategy object per configured bot before any grant stores a stable heap pointer.
  std::vector<StrategyEntry> strategies;
  strategies.reserve(registrations.size());
  for (auto& registration : registrations) {
    strategies.push_back(
        StrategyEntry{std::move(registration.bot_id), std::move(registration.strategy)});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Build source-major, subscription-ID-minor dispatch order and one O(1) range per source.
  std::vector<Grant> grants;
  grants.reserve(configuration.subscriptions().subscriptions().size());
  std::vector<std::size_t> source_offsets(policy.source_capacity() + 1U, 0U);
  for (const auto& source : policy.sources()) {
    const auto source_index = static_cast<std::size_t>(source.ordinal().value() - 1U);
    source_offsets[source_index] = grants.size();
    const auto source_begin = grants.size();
    const auto& definition = source.definition();
    for (const auto& subscription : configuration.subscriptions().subscriptions()) {
      if (subscription.venue_id != definition.venue_id ||
          subscription.instrument_id != definition.instrument_id ||
          subscription.channel != market_data::SubscriptionChannel::OrderBook) {
        continue;
      }

      const auto strategy =
          std::lower_bound(strategies.begin(), strategies.end(), subscription.bot_id,
                           [](const StrategyEntry& entry, const model::BotId& bot_id) {
                             return entry.bot_id < bot_id;
                           });
      const auto* const attribution = configuration.organization().find_bot(subscription.bot_id);
      if (strategy == strategies.end() || strategy->bot_id != subscription.bot_id ||
          attribution == nullptr) {
        return failure<BotRuntime>(DomainErrorCode::StrategyNotConfigured,
                                   "bot_runtime.grants.bot_id");
      }
      grants.push_back(Grant{source.ordinal(),
                             trace::RuntimeTraceSource::from_runtime_source(source), subscription,
                             *attribution, strategy->strategy.get()});
    }
    const auto matching_count = grants.size() - source_begin;
    if (matching_count != static_cast<std::size_t>(source.matching_subscription_count())) {
      return failure<BotRuntime>(DomainErrorCode::InvalidRelationship,
                                 "bot_runtime.grants.matching_subscription_count");
    }
    source_offsets[source_index + 1U] = grants.size();
  }

  return model::Result<BotRuntime>::success(BotRuntime{
      configuration.fingerprint(), policy.fingerprint(), policy.limits().maximum_callbacks_per_turn,
      policy.limits().callback_budget_nanoseconds, measurement_clock, trace_sink, diagnostics,
      std::move(strategies), std::move(grants), std::move(source_offsets), counter_seed});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Mint one turn-scoped proof only after worst-case fan-out and evidence checks succeed.
model::Result<BotDispatchPlan>
BotRuntime::preflight(model::MarketSourceOrdinal source_ordinal, model::TurnOrdinal turn_ordinal,
                      std::uint32_t maximum_events_per_subscription) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // A post-callback fault or recursive planning attempt closes the dispatch boundary before it can
  // authorize another market-state mutation.
  auto healthy = require_healthy();
  if (!healthy) {
    return model::Result<BotDispatchPlan>::failure(healthy.error());
  }
  if (dispatch_active_) {
    return failure<BotDispatchPlan>(DomainErrorCode::DispatchReentryDetected,
                                    "bot_runtime.preflight_reentry");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve the exact canonical source range and reject invalid worst-case event counts.
  const auto source_index = static_cast<std::size_t>(source_ordinal.value() - 1U);
  if (source_index + 1U >= source_offsets_.size()) {
    return failure<BotDispatchPlan>(DomainErrorCode::RuntimeSourceNotConfigured,
                                    "bot_runtime.source_ordinal");
  }
  if (maximum_events_per_subscription == 0U || maximum_events_per_subscription > 2U) {
    return failure<BotDispatchPlan>(DomainErrorCode::InvalidValue,
                                    "bot_runtime.maximum_events_per_subscription");
  }

  const auto matching = source_offsets_[source_index + 1U] - source_offsets_[source_index];
  if (matching > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return failure<BotDispatchPlan>(DomainErrorCode::DispatchCapacityExceeded,
                                    "bot_runtime.matching_subscriptions");
  }
  const auto maximum_callbacks =
      static_cast<std::uint64_t>(matching) * maximum_events_per_subscription;
  if (maximum_callbacks > maximum_callbacks_per_turn_) {
    return failure<BotDispatchPlan>(DomainErrorCode::DispatchCapacityExceeded,
                                    "bot_runtime.maximum_callbacks_per_turn");
  }

  const auto last = last_callback_ordinal_ ? last_callback_ordinal_->value() : 0U;
  if (maximum_callbacks > std::numeric_limits<std::uint64_t>::max() - last) {
    return failure<BotDispatchPlan>(DomainErrorCode::CallbackCounterExhausted, "callback_ordinal");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reserve worst-case callback plus first-reentry trace headroom before the market owner mutates.
  const auto maximum_trace_records = maximum_callbacks * 2U;
  if (maximum_trace_records > std::numeric_limits<std::uint32_t>::max()) {
    return failure<BotDispatchPlan>(DomainErrorCode::DispatchCapacityExceeded,
                                    "bot_runtime.maximum_callback_trace_records");
  }
  auto trace_capacity = trace_sink_->preflight(static_cast<std::uint32_t>(maximum_trace_records));
  if (!trace_capacity) {
    return model::Result<BotDispatchPlan>::failure(trace_capacity.error());
  }
  if (completed_dispatch_count_ == std::numeric_limits<std::uint64_t>::max()) {
    return failure<BotDispatchPlan>(DomainErrorCode::CounterExhausted,
                                    "bot_runtime.completed_dispatch_count");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Capture only immutable routing and current counter predecessors; preflight itself changes no
  // BotRuntime state, so failed domain processing requires no rollback mutation.
  return model::Result<BotDispatchPlan>::success(BotDispatchPlan{
      *this, source_ordinal, turn_ordinal, static_cast<std::uint32_t>(matching),
      maximum_events_per_subscription, static_cast<std::uint32_t>(maximum_callbacks),
      last_callback_ordinal_, completed_dispatch_count_});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Reject later work with stable precedence once post-callback evidence has latched a fault.
model::Result<void> BotRuntime::require_healthy() const {
  if (status_.canonical_trace_failure_latched) {
    return failure<void>(DomainErrorCode::RuntimeEvidenceExhausted,
                         "bot_runtime.canonical_trace_evidence");
  }
  if (status_.callback_clock_regression_latched) {
    return failure<void>(DomainErrorCode::InvalidTimestampOrder,
                         "bot_runtime.callback_clock_regression");
  }
  if (status_.diagnostic_evidence_failure_latched) {
    return failure<void>(DomainErrorCode::RuntimeEvidenceExhausted,
                         "bot_runtime.diagnostic_evidence");
  }
  return model::Result<void>::success();
}

// --------------------------------------------------------
// Preserve the coordinator rollback seam without mutating observational preflight state.
void BotRuntime::cancel(const BotDispatchPlan& plan) noexcept { static_cast<void>(plan); }

// --------------------------------------------------------
// Recheck every private plan binding immediately before dispatch preparation.
model::Result<void> BotRuntime::validate_plan(const BotDispatchPlan& plan) const {
  if (plan.owner_ != this) {
    return failure<void>(DomainErrorCode::InvalidRelationship, "bot_dispatch_plan.owner");
  }
  if (plan.dispatch_counter_predecessor_ != completed_dispatch_count_) {
    return failure<void>(DomainErrorCode::InvalidRelationship, "bot_dispatch_plan.freshness");
  }
  if (plan.callback_counter_predecessor_ != last_callback_ordinal_) {
    return failure<void>(DomainErrorCode::InvalidRelationship,
                         "bot_dispatch_plan.callback_counter_predecessor");
  }

  const auto source_index = static_cast<std::size_t>(plan.source_ordinal_.value() - 1U);
  if (source_index + 1U >= source_offsets_.size()) {
    return failure<void>(DomainErrorCode::RuntimeSourceNotConfigured,
                         "bot_dispatch_plan.source_ordinal");
  }
  const auto matching = source_offsets_[source_index + 1U] - source_offsets_[source_index];
  const auto maximum_callbacks =
      static_cast<std::uint64_t>(matching) * plan.maximum_events_per_subscription_;
  if (matching != plan.matching_subscription_count_ ||
      maximum_callbacks != plan.maximum_callback_count_ ||
      plan.maximum_events_per_subscription_ == 0U || plan.maximum_events_per_subscription_ > 2U) {
    return failure<void>(DomainErrorCode::InvalidRelationship, "bot_dispatch_plan.fanout");
  }
  return model::Result<void>::success();
}

// --------------------------------------------------------
// Convert nested strategy dispatch into the shared callback-local re-entry path.
model::Result<BotDispatchReport> BotRuntime::reject_reentry() {
  auto recorded = record_reentry(false);
  if (!recorded) {
    return model::Result<BotDispatchReport>::failure(recorded.error());
  }
  return failure<BotDispatchReport>(DomainErrorCode::DispatchReentryDetected,
                                    "bot_runtime.dispatch_reentry");
}

// --------------------------------------------------------
// Expose owner-drive recursion recording without granting direct trace mutation.
model::Result<void> BotRuntime::record_owner_reentry() noexcept { return record_reentry(true); }

// --------------------------------------------------------
// Consume at most one prebuilt canonical slot and coalesce all callback-local attempts.
model::Result<void> BotRuntime::record_reentry(bool owner_reentry) noexcept {
  const auto error_code = owner_reentry ? DomainErrorCode::ExecutorReentryDetected
                                        : DomainErrorCode::DispatchReentryDetected;
  const auto error_field =
      owner_reentry ? "bot_runtime.owner_reentry" : "bot_runtime.dispatch_reentry";
  if (active_grant_ == nullptr || active_prepared_callback_ == nullptr || !active_turn_ordinal_ ||
      !active_callback_ordinal_) {
    return failure<void>(error_code, error_field);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Count every callback-local attempt before the first evidence append so a failed canonical
  // append still leaves a complete coalesced diagnostic observation after callback return.
  if (active_reentry_attempts_ == std::numeric_limits<std::uint64_t>::max()) {
    return failure<void>(DomainErrorCode::CounterExhausted, "bot_runtime.reentry_count");
  }
  ++active_reentry_attempts_;
  if (!active_reentry_diagnostic_kind_) {
    active_reentry_diagnostic_kind_ = owner_reentry
                                          ? RuntimeDiagnosticKind::OwnerReentryDetected
                                          : RuntimeDiagnosticKind::DispatchReentryDetected;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Only the first nested attempt consumes the prebuilt callback-scoped canonical reservation;
  // its kind fixes both canonical reason and coalesced diagnostic kind for mixed later attempts.
  if (!active_reentry_traced_ && !active_reentry_trace_failed_) {
    const auto failure_reason = owner_reentry
                                    ? trace::RuntimeTraceFailureReason::OwnerDriveReentry
                                    : trace::RuntimeTraceFailureReason::StrategyDispatchReentry;
    active_prepared_callback_->reentry_trace_fields.failure_reason = failure_reason;
    auto appended = trace_sink_->append(trace::RuntimeTraceEventKind::ReentryDetected,
                                        std::move(active_prepared_callback_->reentry_trace_fields));
    if (!appended) {
      active_reentry_trace_failed_ = true;
      status_.canonical_trace_failure_latched = true;
      return model::Result<void>::failure(appended.error());
    }
    active_reentry_traced_ = true;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical evidence coalesces at one record while the bounded aggregate is published only after
  // the callback returns. A prior append failure is not retried with moved trace fields.
  return failure<void>(error_code, error_field);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Validate a preflighted outcome, prepare complete evidence, and run canonical callback fan-out.
model::Result<BotDispatchReport>
BotRuntime::dispatch(const BotDispatchPlan& plan, const market_data::MarketTurnOutcome& outcome) {
  if (dispatch_active_) {
    return reject_reentry();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Fail closed before inspecting a new proof after any previously latched post-callback fault.
  auto healthy = require_healthy();
  if (!healthy) {
    return model::Result<BotDispatchReport>::failure(healthy.error());
  }
  auto valid_plan = validate_plan(plan);
  if (!valid_plan) {
    return model::Result<BotDispatchReport>::failure(valid_plan.error());
  }
  if (outcome.source_ordinal() != plan.source_ordinal_) {
    return failure<BotDispatchReport>(DomainErrorCode::InvalidRelationship,
                                      "bot_runtime.outcome_source_plan");
  }
  if (outcome.turn_ordinal() != plan.turn_ordinal_) {
    return failure<BotDispatchReport>(DomainErrorCode::InvalidRelationship,
                                      "bot_runtime.outcome_turn_plan");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate the exact event shape against the plan minted before market-state mutation.
  const bool has_state = outcome.state_event().has_value();
  const bool has_market = outcome.market_event().has_value();
  const auto event_count =
      static_cast<std::uint32_t>(has_state) + static_cast<std::uint32_t>(has_market);
  if (event_count > plan.maximum_events_per_subscription_) {
    return failure<BotDispatchReport>(DomainErrorCode::DispatchCapacityExceeded,
                                      "bot_runtime.outcome_event_count");
  }
  if (!has_state && !has_market) {
    if (outcome.expected_callback_count() != 0U ||
        outcome.reserved_callback_trace_records() != 0U) {
      return failure<BotDispatchReport>(DomainErrorCode::DispatchCapacityExceeded,
                                        "bot_runtime.outcome_callback_count");
    }
    ++completed_dispatch_count_;
    return model::Result<BotDispatchReport>::success(
        BotDispatchReport{0U, 0U, 0U, 0U, 0U, 0U, std::nullopt, std::nullopt});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Both event forms, when present, must identify the same configured source and owner turn.
  const auto source_ordinal = outcome_source(outcome);
  const auto turn_ordinal = outcome_turn(outcome);
  if (!source_ordinal) {
    return failure<BotDispatchReport>(DomainErrorCode::RuntimeSourceNotConfigured,
                                      "bot_runtime.outcome_source");
  }
  if (!turn_ordinal) {
    return failure<BotDispatchReport>(DomainErrorCode::InvalidRelationship,
                                      "bot_runtime.outcome_turn");
  }
  if (source_ordinal.value() != outcome.source_ordinal()) {
    return failure<BotDispatchReport>(DomainErrorCode::InvalidRelationship,
                                      "bot_runtime.outcome_event_source");
  }
  if (turn_ordinal.value() != outcome.turn_ordinal()) {
    return failure<BotDispatchReport>(DomainErrorCode::InvalidRelationship,
                                      "bot_runtime.outcome_event_turn");
  }
  if (has_market && !outcome.ready_book()) {
    return failure<BotDispatchReport>(DomainErrorCode::MarketNotReady, "bot_runtime.ready_book");
  }

  const auto exact_callbacks = plan.matching_subscription_count_ * event_count;
  const auto exact_trace_records = static_cast<std::uint64_t>(exact_callbacks) * 2U;
  if (exact_callbacks > plan.maximum_callback_count_ ||
      outcome.expected_callback_count() != exact_callbacks ||
      exact_trace_records > std::numeric_limits<std::uint32_t>::max() ||
      outcome.reserved_callback_trace_records() != exact_trace_records) {
    return failure<BotDispatchReport>(DomainErrorCode::DispatchCapacityExceeded,
                                      "bot_runtime.outcome_callback_count");
  }
  auto capacity = trace_sink_->preflight(outcome.reserved_callback_trace_records());
  if (!capacity) {
    return model::Result<BotDispatchReport>::failure(capacity.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Build and validate the complete callback and possible first-reentry trace set before invoking
  // any strategy. Owning trace identifiers can allocate here; the benchmark must report these
  // preparation allocations rather than claiming an allocation-free complete dispatch turn.
  const auto source_index = static_cast<std::size_t>(source_ordinal->value() - 1U);
  const auto begin = source_offsets_[source_index];
  const auto end = source_offsets_[source_index + 1U];
  prepared_callbacks_.clear();
  std::optional<model::CallbackOrdinal> predicted = last_callback_ordinal_;
  const auto prepare = [&](Grant& grant, bool state_callback) -> model::Result<void> {
    auto ordinal =
        predicted
            ? predicted->next()
            : model::Result<model::CallbackOrdinal>::success(model::CallbackOrdinal::initial());
    if (!ordinal) {
      return model::Result<void>::failure(ordinal.error());
    }
    predicted = ordinal.value();
    auto fields = state_callback
                      ? state_callback_fields(grant, outcome.state_event().value(), ordinal.value())
                      : market_callback_fields(grant, outcome.market_event().value(),
                                               outcome.ready_book().value(), ordinal.value());
    auto nested_fields = reentry_fields(grant, turn_ordinal.value(), ordinal.value());
    const auto kind = state_callback ? trace::RuntimeTraceEventKind::StateCallback
                                     : trace::RuntimeTraceEventKind::MarketCallback;
    auto valid = trace_sink_->validate(kind, fields);
    if (!valid) {
      return valid;
    }
    nested_fields.failure_reason = trace::RuntimeTraceFailureReason::StrategyDispatchReentry;
    auto dispatch_reentry_valid =
        trace_sink_->validate(trace::RuntimeTraceEventKind::ReentryDetected, nested_fields);
    if (!dispatch_reentry_valid) {
      return dispatch_reentry_valid;
    }
    nested_fields.failure_reason = trace::RuntimeTraceFailureReason::OwnerDriveReentry;
    auto owner_reentry_valid =
        trace_sink_->validate(trace::RuntimeTraceEventKind::ReentryDetected, nested_fields);
    if (!owner_reentry_valid) {
      return owner_reentry_valid;
    }
    nested_fields.failure_reason = trace::RuntimeTraceFailureReason::None;
    prepared_callbacks_.push_back(PreparedCallback{&grant, ordinal.value(), std::move(fields),
                                                   std::move(nested_fields), state_callback});
    return model::Result<void>::success();
  };

  if (has_state) {
    for (std::size_t index = begin; index < end; ++index) {
      auto prepared = prepare(grants_[index], true);
      if (!prepared) {
        return model::Result<BotDispatchReport>::failure(prepared.error());
      }
    }
  }
  if (has_market) {
    for (std::size_t index = begin; index < end; ++index) {
      auto prepared = prepare(grants_[index], false);
      if (!prepared) {
        return model::Result<BotDispatchReport>::failure(prepared.error());
      }
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Invoke every prepared callback synchronously; trace publication occurs immediately before
  // entry so a nested dispatch can append its prebuilt reserved re-entry record in causal order.
  BotDispatchReport report{0U, 0U, 0U, 0U, 0U, 0U, std::nullopt, std::nullopt};
  const auto dropped_before = diagnostics_->dropped_count();
  ++completed_dispatch_count_;
  dispatch_active_ = true;
  for (auto& prepared : prepared_callbacks_) {
    const auto kind = prepared.state_callback ? trace::RuntimeTraceEventKind::StateCallback
                                              : trace::RuntimeTraceEventKind::MarketCallback;
    auto appended = trace_sink_->append(kind, std::move(prepared.trace_fields));
    if (!appended) {
      status_.canonical_trace_failure_latched = true;
      dispatch_active_ = false;
      prepared_callbacks_.clear();
      return model::Result<BotDispatchReport>::failure(appended.error());
    }

    last_callback_ordinal_ = prepared.callback_ordinal;
    if (!report.first_callback_ordinal) {
      report.first_callback_ordinal = prepared.callback_ordinal;
    }
    report.last_callback_ordinal = prepared.callback_ordinal;
    active_grant_ = prepared.grant;
    active_prepared_callback_ = &prepared;
    active_turn_ordinal_ = turn_ordinal;
    active_callback_ordinal_ = prepared.callback_ordinal;
    active_reentry_traced_ = false;
    active_reentry_trace_failed_ = false;
    active_reentry_attempts_ = 0U;
    active_reentry_diagnostic_kind_.reset();

    BotContext context{prepared.grant->attribution, prepared.grant->subscription.id,
                       prepared.callback_ordinal, configuration_fingerprint_,
                       runtime_policy_fingerprint_};
    const auto started = measurement_clock_->processing_now();
    if (prepared.state_callback) {
      prepared.grant->strategy->on_market_state(outcome.state_event().value(), context);
      ++report.state_callbacks;
    } else {
      prepared.grant->strategy->on_market_data(outcome.market_event().value(),
                                               outcome.ready_book().value(), context);
      ++report.market_callbacks;
    }
    const auto finished = measurement_clock_->processing_now();

    const auto reentry_attempts = active_reentry_attempts_;
    const auto reentry_diagnostic_kind = active_reentry_diagnostic_kind_;

    active_grant_ = nullptr;
    active_prepared_callback_ = nullptr;
    active_turn_ordinal_.reset();
    active_callback_ordinal_.reset();
    active_reentry_traced_ = false;
    active_reentry_trace_failed_ = false;
    active_reentry_attempts_ = 0U;
    active_reentry_diagnostic_kind_.reset();

    RuntimeDiagnosticFields diagnostic;
    diagnostic.turn_ordinal = turn_ordinal;
    diagnostic.callback_ordinal = prepared.callback_ordinal;
    if (reentry_attempts != 0U && reentry_diagnostic_kind) {
      diagnostic.occurrence_count = reentry_attempts;
      append_callback_diagnostic(*reentry_diagnostic_kind, diagnostic, report);
      diagnostic.occurrence_count = 1U;
    }
    if (finished.nanoseconds() < started.nanoseconds()) {
      ++report.callback_clock_regressions;
      status_.callback_clock_regression_latched = true;
      diagnostic.observed_value = finished.nanoseconds();
      diagnostic.limit_value = started.nanoseconds();
      append_callback_diagnostic(RuntimeDiagnosticKind::CallbackClockRegression, diagnostic,
                                 report);
    } else {
      const auto duration = finished.nanoseconds() - started.nanoseconds();
      if (duration > callback_budget_nanoseconds_) {
        diagnostic.observed_value = duration;
        diagnostic.limit_value = callback_budget_nanoseconds_;
        append_callback_diagnostic(RuntimeDiagnosticKind::CallbackBudgetExceeded, diagnostic,
                                   report);
        ++report.callback_budget_exceeded;
      }
    }
  }
  dispatch_active_ = false;
  report.diagnostic_observations_dropped = diagnostics_->dropped_count() - dropped_before;
  prepared_callbacks_.clear();
  return model::Result<BotDispatchReport>::success(report);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Treat diagnostic-sink failures as a later fail-closed status without truncating this fan-out.
void BotRuntime::append_callback_diagnostic(RuntimeDiagnosticKind kind,
                                            const RuntimeDiagnosticFields& fields,
                                            BotDispatchReport& report) noexcept {
  auto appended = diagnostics_->append(kind, fields);
  if (!appended) {
    status_.diagnostic_evidence_failure_latched = true;
    ++report.diagnostic_evidence_failures;
  }
}

// --------------------------------------------------------

} // namespace aegis::runtime
