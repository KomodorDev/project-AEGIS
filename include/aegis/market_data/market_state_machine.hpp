// Purpose: classify normalized market input, build books transactionally, and publish deterministic
// readiness/event outcomes for later strategy dispatch.

#pragma once

#include "aegis/market_data/market_event.hpp"
#include "aegis/market_data/order_book.hpp"
#include "aegis/model/instrument_metadata.hpp"
#include "aegis/model/result.hpp"
#include "aegis/runtime/runtime_policy.hpp"
#include "aegis/trace/runtime_trace.hpp"

#include <cstdint>
#include <optional>

namespace aegis::market_data {

// ########################################################################
// Accepted owner turns supply only executor identities. Callback fan-out remains a policy-derived
// property of the configured source and cannot be authored by an ingress or coordinator caller.
struct AcceptedMarketTurnContext {
  model::AdmissionOrdinal admission_ordinal;
  model::TurnOrdinal turn_ordinal;
  model::ProcessingTimestamp processing_timestamp;

  // --------------------------------------------------------
  // Structural equality pins deterministic owner-turn replay inputs.
  friend bool operator==(const AcceptedMarketTurnContext&,
                         const AcceptedMarketTurnContext&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Owner controls have no accepted ingress identity and cannot alter configured callback fan-out.
struct OwnerMarketTurnContext {
  model::TurnOrdinal turn_ordinal;
  model::ProcessingTimestamp processing_timestamp;

  // --------------------------------------------------------
  // Structural equality pins deterministic owner-control replay inputs.
  friend bool operator==(const OwnerMarketTurnContext&, const OwnerMarketTurnContext&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Sanitized attributable parse failures retain only their assigned disposition and trusted
// envelope identity; malformed bytes never cross this boundary.
struct AttributableMarketFailure {
  MarketSourceIdentity source;
  model::SessionEpoch session_epoch;
  model::ReceiveSequence receive_sequence;
  model::ReceiveTimestamp receive_timestamp;
  trace::RuntimeInputDisposition disposition;

  // --------------------------------------------------------
  // Structural equality supports exact malformed-input containment assertions.
  friend bool operator==(const AttributableMarketFailure&,
                         const AttributableMarketFailure&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// BookIdentity advances generation only for snapshots while every commit advances the global
// revision; its checked helpers make exhaustion testable without billions of commits.
class BookIdentity final {
public:

  // --------------------------------------------------------
  // Validate a restored committed identity before it can seed checked advancement.
  [[nodiscard]] static model::Result<BookIdentity> from_committed(model::BookGeneration generation,
                                                                  model::BookRevision revision);

  // --------------------------------------------------------
  // Return the first snapshot identity.
  [[nodiscard]] static constexpr BookIdentity initial() noexcept {
    return BookIdentity{model::BookGeneration::initial(), model::BookRevision::initial()};
  }

  // --------------------------------------------------------
  // Start a new snapshot generation and advance the global revision exactly once.
  [[nodiscard]] model::Result<BookIdentity> next_snapshot() const;

  // --------------------------------------------------------
  // Advance only the global revision for a valid delta commit.
  [[nodiscard]] model::Result<BookIdentity> next_delta() const;

  // --------------------------------------------------------
  [[nodiscard]] model::BookGeneration generation() const noexcept { return generation_; }

  // --------------------------------------------------------
  [[nodiscard]] model::BookRevision revision() const noexcept { return revision_; }

  // --------------------------------------------------------
  friend bool operator==(const BookIdentity&, const BookIdentity&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  constexpr BookIdentity(model::BookGeneration generation, model::BookRevision revision) noexcept
      : generation_{generation}, revision_{revision} {}

  // --------------------------------------------------------
  model::BookGeneration generation_;
  model::BookRevision revision_;
};

// ########################################################################

// ########################################################################
// The integrity seam receives only normalized input and an immutable complete candidate. M2's
// fixture implementation interprets the adapter verdict without embedding a venue algorithm.
using MarketIntegrityValidator = model::Result<void> (*)(const NormalizedMarketUpdate&,
                                                         ReadyBookView);

// ########################################################################

// --------------------------------------------------------
// Enforce the deterministic recorded-fixture verdict against a complete immutable candidate.
[[nodiscard]] model::Result<void>
validate_recorded_fixture_integrity(const NormalizedMarketUpdate& update, ReadyBookView candidate);

// --------------------------------------------------------

// ########################################################################
// One successful turn outcome owns post-commit events in dispatch order and borrows a Ready view
// only when a market callback is permitted. It contains no strategy or callback state.
class MarketTurnOutcome final {
public:

  // --------------------------------------------------------
  // Identify the configured source that produced this outcome, including zero-event turns.
  [[nodiscard]] model::MarketSourceOrdinal source_ordinal() const noexcept {
    return source_ordinal_;
  }

  // --------------------------------------------------------
  // Identify the owner turn that produced this outcome, including zero-event turns.
  [[nodiscard]] model::TurnOrdinal turn_ordinal() const noexcept { return turn_ordinal_; }

  // --------------------------------------------------------
  // Return the resulting explicit readiness.
  [[nodiscard]] MarketReadiness readiness() const noexcept { return readiness_; }

  // --------------------------------------------------------
  // Return the stable input classification when this was an input-bearing turn.
  [[nodiscard]] std::optional<trace::RuntimeInputDisposition> disposition() const noexcept {
    return disposition_;
  }

  // --------------------------------------------------------
  // Borrow a sanitized state transition for dispatch before any market event.
  [[nodiscard]] const std::optional<MarketStateEvent>& state_event() const noexcept {
    return state_event_;
  }

  // --------------------------------------------------------
  // Borrow a post-commit market event, present only for an applied snapshot or delta.
  [[nodiscard]] const std::optional<MarketEvent>& market_event() const noexcept {
    return market_event_;
  }

  // --------------------------------------------------------
  // Borrow the coherent current book only alongside a post-commit market event.
  [[nodiscard]] const std::optional<ReadyBookView>& ready_book() const noexcept {
    return ready_book_;
  }

  // --------------------------------------------------------
  // Distinguish a successful book swap from ignored, rejected, and control turns.
  [[nodiscard]] bool book_committed() const noexcept { return book_committed_; }

  // --------------------------------------------------------
  // Return the exact callback count implied by present state and market events across all grants.
  [[nodiscard]] std::uint32_t expected_callback_count() const noexcept {
    return expected_callback_count_;
  }

  // --------------------------------------------------------
  // Return callback records plus one reserved first re-entry record for every expected callback.
  [[nodiscard]] std::uint32_t reserved_callback_trace_records() const noexcept {
    return reserved_callback_trace_records_;
  }

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only the transactional owner can couple private event factories to its committed state.
  friend class MarketStateMachine;

  // ########################################################################

  // --------------------------------------------------------
  // Assemble one fully published owner-turn result after trace and mutation invariants hold.
  MarketTurnOutcome(model::MarketSourceOrdinal source_ordinal, model::TurnOrdinal turn_ordinal,
                    MarketReadiness readiness,
                    std::optional<trace::RuntimeInputDisposition> disposition,
                    std::optional<MarketStateEvent> state_event,
                    std::optional<MarketEvent> market_event,
                    std::optional<ReadyBookView> ready_book, bool book_committed,
                    std::uint32_t expected_callback_count,
                    std::uint32_t reserved_callback_trace_records)
      : source_ordinal_{source_ordinal}, turn_ordinal_{turn_ordinal}, readiness_{readiness},
        disposition_{disposition}, state_event_{std::move(state_event)},
        market_event_{std::move(market_event)}, ready_book_{std::move(ready_book)},
        book_committed_{book_committed}, expected_callback_count_{expected_callback_count},
        reserved_callback_trace_records_{reserved_callback_trace_records} {}

  // --------------------------------------------------------
  model::MarketSourceOrdinal source_ordinal_;
  model::TurnOrdinal turn_ordinal_;
  MarketReadiness readiness_;
  std::optional<trace::RuntimeInputDisposition> disposition_;
  std::optional<MarketStateEvent> state_event_;
  std::optional<MarketEvent> market_event_;
  std::optional<ReadyBookView> ready_book_;
  bool book_committed_;
  std::uint32_t expected_callback_count_;
  std::uint32_t reserved_callback_trace_records_;
};

// ########################################################################

// ########################################################################
// MarketStateMachine is the dedicated owner of mutable source continuity, current/scratch books,
// readiness, book counters, and freshness anchors for exactly one configured source.
class MarketStateMachine final {
public:

  // --------------------------------------------------------
  // Validate policy membership/source/metadata coherence and preallocate fixed owner state.
  [[nodiscard]] static model::Result<MarketStateMachine>
  create(const runtime::RuntimePolicy& policy, const runtime::RuntimeSource& source,
         model::InstrumentMetadata metadata,
         MarketIntegrityValidator integrity_validator = validate_recorded_fixture_integrity);

  // --------------------------------------------------------
  // Mutable owner state cannot be copied; a one-time move supports factory publication.
  MarketStateMachine(const MarketStateMachine&) = delete;
  MarketStateMachine& operator=(const MarketStateMachine&) = delete;
  MarketStateMachine(MarketStateMachine&&) noexcept = default;
  MarketStateMachine& operator=(MarketStateMachine&&) noexcept = default;

  // --------------------------------------------------------
  // Publish the required initial Synchronizing transition without fabricating ingress context.
  [[nodiscard]] model::Result<MarketTurnOutcome> initialize(const OwnerMarketTurnContext& context,
                                                            trace::RuntimeTraceSink& trace_sink);

  // --------------------------------------------------------
  // Clear source continuity and require a fresh snapshot, retaining hidden historical book bytes.
  [[nodiscard]] model::Result<MarketTurnOutcome>
  resynchronize(const OwnerMarketTurnContext& context, trace::RuntimeTraceSink& trace_sink);

  // --------------------------------------------------------
  // Classify and transactionally apply one normalized snapshot or delta.
  [[nodiscard]] model::Result<MarketTurnOutcome> process(NormalizedMarketUpdate update,
                                                         const AcceptedMarketTurnContext& context,
                                                         trace::RuntimeTraceSink& trace_sink);

  // --------------------------------------------------------
  // Apply deterministic session-age policy and clear continuity only for a newer session.
  [[nodiscard]] model::Result<MarketTurnOutcome> process(const SessionStarted& control,
                                                         const AcceptedMarketTurnContext& context,
                                                         trace::RuntimeTraceSink& trace_sink);

  // --------------------------------------------------------
  // Evaluate the explicit freshness timestamp without reading an ambient clock.
  [[nodiscard]] model::Result<MarketTurnOutcome> process(const StalenessCheck& control,
                                                         const AcceptedMarketTurnContext& context,
                                                         trace::RuntimeTraceSink& trace_sink);

  // --------------------------------------------------------
  // Contain one attributable malformed or unsupported active-stream frame without raw payload.
  [[nodiscard]] model::Result<MarketTurnOutcome> reject(const AttributableMarketFailure& failure,
                                                        const AcceptedMarketTurnContext& context,
                                                        trace::RuntimeTraceSink& trace_sink);

  // --------------------------------------------------------
  // Consume one ordered admission-loss fence and require snapshot recovery.
  [[nodiscard]] model::Result<MarketTurnOutcome>
  source_discontinuity(model::AdmissionOrdinal failed_admission,
                       const OwnerMarketTurnContext& context, trace::RuntimeTraceSink& trace_sink);

  // --------------------------------------------------------
  // Return absence until initialization publishes the first explicit state.
  [[nodiscard]] std::optional<MarketReadiness> readiness() const noexcept { return readiness_; }

  // --------------------------------------------------------
  // Expose a book only while the source is explicitly Ready.
  [[nodiscard]] model::Result<ReadyBookView> ready_book() const;

  // --------------------------------------------------------
  // Return the last committed generation/revision pair, absent before the first snapshot.
  [[nodiscard]] std::optional<BookIdentity> book_identity() const noexcept {
    return book_identity_;
  }

  // --------------------------------------------------------
  // Return the continuity anchor retained for duplicate and predecessor classification.
  [[nodiscard]] std::optional<model::SessionEpoch> active_session() const noexcept {
    return active_session_;
  }

  // --------------------------------------------------------
  [[nodiscard]] std::optional<model::SequenceNumber> last_source_sequence() const noexcept {
    return last_source_sequence_;
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish only factory-validated source, metadata, limits, and integrity policy.
  MarketStateMachine(MarketSourceIdentity source, trace::RuntimeTraceSource trace_source,
                     model::InstrumentMetadata metadata, std::size_t retained_depth,
                     std::size_t maximum_changes_per_update,
                     std::uint64_t stale_threshold_nanoseconds,
                     std::uint32_t matching_subscription_count,
                     std::uint32_t maximum_callbacks_per_turn,
                     trace::RuntimeTraceProvenance expected_trace_provenance,
                     MarketIntegrityValidator integrity_validator)
      : source_{std::move(source)}, trace_source_{std::move(trace_source)},
        metadata_{std::move(metadata)}, retained_depth_{retained_depth},
        maximum_changes_per_update_{maximum_changes_per_update},
        stale_threshold_nanoseconds_{stale_threshold_nanoseconds},
        matching_subscription_count_{matching_subscription_count},
        maximum_callbacks_per_turn_{maximum_callbacks_per_turn},
        expected_trace_provenance_{std::move(expected_trace_provenance)},
        integrity_validator_{integrity_validator} {}

  // --------------------------------------------------------
  MarketSourceIdentity source_;
  trace::RuntimeTraceSource trace_source_;
  model::InstrumentMetadata metadata_;
  std::size_t retained_depth_;
  std::size_t maximum_changes_per_update_;
  std::uint64_t stale_threshold_nanoseconds_;
  std::uint32_t matching_subscription_count_;
  std::uint32_t maximum_callbacks_per_turn_;
  trace::RuntimeTraceProvenance expected_trace_provenance_;
  MarketIntegrityValidator integrity_validator_;

  std::optional<MarketReadiness> readiness_;
  FixedDepthOrderBook current_book_;
  FixedDepthOrderBook scratch_book_;
  std::optional<BookIdentity> book_identity_;
  std::optional<model::SessionEpoch> active_session_;
  std::optional<model::SequenceNumber> last_source_sequence_;
  std::optional<model::Sha256Digest> last_payload_digest_;
  std::optional<model::ProcessingTimestamp> last_commit_time_;
};

// ########################################################################

} // namespace aegis::market_data
