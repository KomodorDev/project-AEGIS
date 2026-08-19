// Purpose: define venue-neutral normalized market commands and immutable post-commit events without
// exposing mutable book storage or venue wire-protocol details.

#pragma once

#include "aegis/market_data/market_limits.hpp"
#include "aegis/model/fixed_point.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// Runtime policy is the sole authority that can supply a validated configured source.
class RuntimeSource;

// ########################################################################

} // namespace aegis::runtime

namespace aegis::market_data {

// ########################################################################
// The transactional market owner is the only publisher of strategy-visible market events.
class MarketStateMachine;

// ########################################################################
// The normalized schema and token bound are repository contracts rather than venue-specific
// protocol choices. Receive and processing values are deliberately outside the payload digest.
inline constexpr std::uint16_t normalized_market_schema_version = 1U;

// ########################################################################
// Readiness is strategy-visible and closed: every value other than these four fails validation.
enum class MarketReadiness : std::uint8_t {
  Synchronizing = 1,
  Ready = 2,
  Stale = 3,
  Invalid = 4,
};

// ########################################################################
// Snapshot and delta meaning remains venue-neutral across fixture and future live adapters.
enum class MarketUpdateKind : std::uint8_t {
  Snapshot = 1,
  Delta = 2,
};

// ########################################################################
// A side is part of each absolute price-level change and never inferred from signed quantity.
enum class BookSide : std::uint8_t {
  Bid = 1,
  Ask = 2,
};

// ########################################################################
// Integrity reports the adapter's deterministic decision without selecting a venue algorithm.
enum class IntegrityVerdict : std::uint8_t {
  Accepted = 1,
  Rejected = 2,
};

// ########################################################################
// Hash an opaque bounded integrity token into a fixed-width identity so normalized commands never
// retain adapter-authored text or unbounded storage.
class IntegrityTokenIdentity final {
public:

  // --------------------------------------------------------
  // Hash a nonempty bounded token exactly as supplied by the adapter boundary.
  [[nodiscard]] static model::Result<IntegrityTokenIdentity> from_token(std::string_view token);

  // --------------------------------------------------------
  // Borrow the exact fixed-width identity used by canonical payload hashing.
  [[nodiscard]] constexpr const model::Sha256Digest& digest() const noexcept { return digest_; }

  // --------------------------------------------------------
  // Compare identities by all digest bytes.
  friend constexpr bool operator==(const IntegrityTokenIdentity&,
                                   const IntegrityTokenIdentity&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Construct fixed digest bytes only from the bounded token-hashing path owned by this class.
  [[nodiscard]] static constexpr IntegrityTokenIdentity
  from_digest(model::Sha256Digest digest) noexcept {
    return IntegrityTokenIdentity{digest};
  }

  // --------------------------------------------------------
  // Restrict digest construction to explicit token hashing or trusted canonical decoding.
  explicit constexpr IntegrityTokenIdentity(model::Sha256Digest digest) noexcept
      : digest_{digest} {}

  // --------------------------------------------------------
  model::Sha256Digest digest_;
};

// ########################################################################

// ########################################################################
// Pair the integrity decision with the fixed identity of the token on which it was based.
struct MarketIntegrity {
  IntegrityVerdict verdict;
  IntegrityTokenIdentity token_identity;

  // --------------------------------------------------------
  // Structural equality retains both decision and token identity.
  friend constexpr bool operator==(const MarketIntegrity&, const MarketIntegrity&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Bind one configured source ordinal to its complete venue-neutral and venue-native attribution.
class MarketSourceIdentity final {
public:

  // --------------------------------------------------------
  // Copy one source only after RuntimePolicy has validated and assigned its canonical ordinal.
  [[nodiscard]] static MarketSourceIdentity
  from_runtime_source(const runtime::RuntimeSource& source);

  // --------------------------------------------------------
  [[nodiscard]] const model::MarketSourceId& source_id() const noexcept { return source_id_; }

  // --------------------------------------------------------
  [[nodiscard]] model::MarketSourceOrdinal source_ordinal() const noexcept {
    return source_ordinal_;
  }

  // --------------------------------------------------------
  [[nodiscard]] const model::VenueId& venue_id() const noexcept { return venue_id_; }

  // --------------------------------------------------------
  [[nodiscard]] const model::InstrumentId& instrument_id() const noexcept { return instrument_id_; }

  // --------------------------------------------------------
  [[nodiscard]] const model::VenueInstrumentId& venue_instrument_id() const noexcept {
    return venue_instrument_id_;
  }

  // --------------------------------------------------------
  // Structural equality compares the complete configured source identity.
  friend bool operator==(const MarketSourceIdentity&, const MarketSourceIdentity&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Restrict tuple assembly to the validated RuntimeSource conversion above.
  MarketSourceIdentity(model::MarketSourceId source_id, model::MarketSourceOrdinal source_ordinal,
                       model::VenueId venue_id, model::InstrumentId instrument_id,
                       model::VenueInstrumentId venue_instrument_id)
      : source_id_{std::move(source_id)}, source_ordinal_{source_ordinal},
        venue_id_{std::move(venue_id)}, instrument_id_{std::move(instrument_id)},
        venue_instrument_id_{std::move(venue_instrument_id)} {}

  // --------------------------------------------------------
  model::MarketSourceId source_id_;
  model::MarketSourceOrdinal source_ordinal_;
  model::VenueId venue_id_;
  model::InstrumentId instrument_id_;
  model::VenueInstrumentId venue_instrument_id_;
};

// ########################################################################

// ########################################################################
// Carry one absolute quantity at an exact price; delta quantity zero means deletion while snapshot
// entries must remain positive after normalization.
struct MarketLevelChange {
  BookSide side;
  model::Price price;
  model::Quantity quantity;

  // --------------------------------------------------------
  // Structural equality supports canonical update and replay comparisons.
  friend bool operator==(const MarketLevelChange&, const MarketLevelChange&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Collect authored normalization inputs so the factory can validate and canonicalize them before
// any value is published to mutable market state.
struct NormalizedMarketUpdateFields {
  MarketSourceIdentity source;
  model::SessionEpoch session_epoch;
  model::SequenceNumber source_sequence;
  std::optional<model::SequenceNumber> predecessor_sequence;
  model::SourceTimestamp source_timestamp;
  model::ReceiveSequence receive_sequence;
  model::ReceiveTimestamp receive_timestamp;
  model::InstrumentMetadataRevision metadata_revision;
  MarketUpdateKind kind;
  MarketIntegrity integrity;
  std::vector<MarketLevelChange> changes;

  // --------------------------------------------------------
  // Structural equality includes timing even though canonical payload identity intentionally does
  // not, keeping transport replay and semantic duplicate identity separately testable.
  friend bool operator==(const NormalizedMarketUpdateFields&,
                         const NormalizedMarketUpdateFields&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Own one completely validated and canonically ordered pre-book update. Its payload digest excludes
// owner timing so semantic duplicates remain identical across equivalent replays.
class NormalizedMarketUpdate final {
public:

  // --------------------------------------------------------
  // Validate enums, level shape, duplicates, and the sealed per-update change bound before hashing.
  [[nodiscard]] static model::Result<NormalizedMarketUpdate>
  create(NormalizedMarketUpdateFields fields, std::size_t maximum_changes);

  // --------------------------------------------------------
  // Borrow the complete immutable source attribution.
  [[nodiscard]] const MarketSourceIdentity& source() const noexcept { return fields_.source; }

  // --------------------------------------------------------
  // Return the source session in which sequence continuity is interpreted.
  [[nodiscard]] model::SessionEpoch session_epoch() const noexcept { return fields_.session_epoch; }

  // --------------------------------------------------------
  // Return the adapter-authored sequence without assuming unit increments.
  [[nodiscard]] model::SequenceNumber source_sequence() const noexcept {
    return fields_.source_sequence;
  }

  // --------------------------------------------------------
  // Return the explicit predecessor used by owner-side continuity classification.
  [[nodiscard]] std::optional<model::SequenceNumber> predecessor_sequence() const noexcept {
    return fields_.predecessor_sequence;
  }

  // --------------------------------------------------------
  // Return the source-authored wall-like event timestamp.
  [[nodiscard]] model::SourceTimestamp source_timestamp() const noexcept {
    return fields_.source_timestamp;
  }

  // --------------------------------------------------------
  // Return the owner-assigned sequence of accepted input.
  [[nodiscard]] model::ReceiveSequence receive_sequence() const noexcept {
    return fields_.receive_sequence;
  }

  // --------------------------------------------------------
  // Return the injected monotonic admission timestamp.
  [[nodiscard]] model::ReceiveTimestamp receive_timestamp() const noexcept {
    return fields_.receive_timestamp;
  }

  // --------------------------------------------------------
  // Return the immutable metadata revision under which values must validate.
  [[nodiscard]] model::InstrumentMetadataRevision metadata_revision() const noexcept {
    return fields_.metadata_revision;
  }

  // --------------------------------------------------------
  // Distinguish complete replacement from absolute-quantity incremental change.
  [[nodiscard]] MarketUpdateKind kind() const noexcept { return fields_.kind; }

  // --------------------------------------------------------
  // Return the adapter's fixed integrity decision and token identity.
  [[nodiscard]] const MarketIntegrity& integrity() const noexcept { return fields_.integrity; }

  // --------------------------------------------------------
  // Borrow changes in canonical side-then-price order.
  [[nodiscard]] const std::vector<MarketLevelChange>& changes() const noexcept {
    return fields_.changes;
  }

  // --------------------------------------------------------
  // Borrow the timing-independent semantic duplicate identity.
  [[nodiscard]] const model::Sha256Digest& payload_digest() const noexcept {
    return payload_digest_;
  }

  // --------------------------------------------------------
  // Structural equality compares the complete normalized transport value and its derived digest.
  friend bool operator==(const NormalizedMarketUpdate&, const NormalizedMarketUpdate&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish only factory-validated fields paired with their derived canonical identity.
  NormalizedMarketUpdate(NormalizedMarketUpdateFields fields, model::Sha256Digest payload_digest)
      : fields_{std::move(fields)}, payload_digest_{payload_digest} {}

  // --------------------------------------------------------
  NormalizedMarketUpdateFields fields_;
  model::Sha256Digest payload_digest_;
};

// ########################################################################

// ########################################################################
// Reset continuity for one configured source using only recorded and owner-assigned identities.
struct SessionStarted {
  MarketSourceIdentity source;
  model::SessionEpoch session_epoch;
  model::SourceTimestamp source_timestamp;
  model::ReceiveSequence receive_sequence;
  model::ReceiveTimestamp receive_timestamp;

  // --------------------------------------------------------
  // Structural equality pins deterministic session-control replay.
  friend bool operator==(const SessionStarted&, const SessionStarted&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Evaluate freshness for exactly one source at an explicit owner processing time; no ambient clock
// read can change readiness outside a command turn.
struct StalenessCheck {
  MarketSourceIdentity source;
  model::SessionEpoch session_epoch;
  model::ReceiveSequence receive_sequence;
  model::ReceiveTimestamp receive_timestamp;
  model::ProcessingTimestamp processing_timestamp;

  // --------------------------------------------------------
  // Structural equality pins the complete explicit freshness input.
  friend bool operator==(const StalenessCheck&, const StalenessCheck&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Keep the three accepted recorded-frame products distinct. Owner-local resynchronization and
// rejected-admission discontinuity are runtime controls rather than parsed market commands.
using NormalizedRecordedMarketCommand =
    std::variant<NormalizedMarketUpdate, SessionStarted, StalenessCheck>;

// ########################################################################

// ########################################################################
// Couple one successful commit to the exact serialized owner turn that made it visible.
struct MarketCommitContext {
  model::AdmissionOrdinal admission_ordinal;
  model::TurnOrdinal turn_ordinal;
  model::ProcessingTimestamp processing_timestamp;
  model::BookGeneration book_generation;
  model::BookRevision book_revision;

  // --------------------------------------------------------
  // Structural equality compares every owner and book position attached to a commit.
  friend bool operator==(const MarketCommitContext&, const MarketCommitContext&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Expose only a successfully committed Ready update. The later book module supplies the separate
// turn-scoped ReadyBookView and retains all mutable level storage.
class MarketEvent final {
public:

  // --------------------------------------------------------
  // Borrow the complete committed normalized update without exposing mutable state.
  [[nodiscard]] const NormalizedMarketUpdate& update() const noexcept { return update_; }

  // --------------------------------------------------------
  // Borrow the serialized owner and committed-book identity.
  [[nodiscard]] const MarketCommitContext& context() const noexcept { return context_; }

  // --------------------------------------------------------
  // A market event is constructible only after a Ready commit.
  [[nodiscard]] static constexpr MarketReadiness readiness() noexcept {
    return MarketReadiness::Ready;
  }

  // --------------------------------------------------------
  // Structural equality supports exact callback-vector comparisons.
  friend bool operator==(const MarketEvent&, const MarketEvent&) = default;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only the transactional book/state owner may publish a post-commit strategy event.
  friend class MarketStateMachine;

  // ########################################################################

  // --------------------------------------------------------
  // Publish only fields the transactional owner already validated before its no-fail commit.
  MarketEvent(NormalizedMarketUpdate update, MarketCommitContext context) noexcept
      : update_{std::move(update)}, context_{context} {}

  // --------------------------------------------------------
  NormalizedMarketUpdate update_;
  MarketCommitContext context_;
};

// ########################################################################

// ########################################################################
// Collect transition fields whose input-specific identities may be absent for startup, explicit
// resynchronization, malformed input, or a discontinuity fence.
struct MarketStateEventFields {
  MarketSourceIdentity source;
  std::optional<model::SessionEpoch> session_epoch;
  std::optional<model::SequenceNumber> source_sequence;
  std::optional<model::ReceiveSequence> receive_sequence;
  std::optional<model::ReceiveTimestamp> receive_timestamp;
  std::optional<model::AdmissionOrdinal> admission_ordinal;
  model::TurnOrdinal turn_ordinal;
  model::ProcessingTimestamp processing_timestamp;
  std::optional<model::InstrumentMetadataRevision> metadata_revision;
  std::optional<model::BookGeneration> book_generation;
  std::optional<model::BookRevision> book_revision;
  std::optional<MarketReadiness> previous_readiness;
  MarketReadiness readiness;

  // --------------------------------------------------------
  // Structural equality compares all transition provenance and optional-presence decisions.
  friend bool operator==(const MarketStateEventFields&, const MarketStateEventFields&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Validate a complete transition profile without granting strategy-publication authority.
[[nodiscard]] model::Result<void>
validate_market_state_transition(const MarketStateEventFields& fields);

// --------------------------------------------------------

// ########################################################################
// Publish sanitized strategy-visible readiness transitions without a book view or raw malformed
// input. Optional book identities describe the last commit but never imply readiness by themselves.
class MarketStateEvent final {
public:

  // --------------------------------------------------------
  // Borrow the complete immutable sanitized transition.
  [[nodiscard]] const MarketStateEventFields& fields() const noexcept { return fields_; }

  // --------------------------------------------------------
  // Structural equality supports exact state-callback replay comparisons.
  friend bool operator==(const MarketStateEvent&, const MarketStateEvent&) = default;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only the owner that applied the accepted transition table may publish this event.
  friend class MarketStateMachine;

  // ########################################################################

  // --------------------------------------------------------
  // Publish only a profile the transactional owner validated before trace or state mutation.
  explicit MarketStateEvent(MarketStateEventFields fields) noexcept : fields_{std::move(fields)} {}

  // --------------------------------------------------------
  MarketStateEventFields fields_;
};

// ########################################################################

// ########################################################################
// The order-book module defines this immutable turn-scoped view; this boundary intentionally owns
// neither book representation nor level storage.
class ReadyBookView;

// ########################################################################

} // namespace aegis::market_data
