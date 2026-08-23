// Purpose: define the closed M4 source-normalized private-order vocabulary, bounded payloads,
// origin envelopes, ingress equality, and pure trade-comparison value without OMS mutation.

#pragma once

#include "aegis/execution/order_request.hpp"
#include "aegis/model/fixed_point.hpp"
#include "aegis/model/m4_provenance.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"
#include "aegis/oms/private_order_identity.hpp"
#include "aegis/recovery/recovery_identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>

namespace aegis::runtime {

// ########################################################################
// Runtime composition is the only factory allowed to attach trusted provenance to source facts.
class PrivateOrderEventFactory;

// ########################################################################

} // namespace aegis::runtime

namespace aegis::oms {

// ########################################################################
// Origin assignments distinguish AEGIS-local facts, ordinary private-source facts, and complete
// authoritative reconciliation rows without treating them as interchangeable message IDs.
enum class PrivateEventOrigin : std::uint8_t {
  Local = 1,
  Venue = 2,
  Reconciliation = 3,
};

// ########################################################################

// ########################################################################
// These nine values are the complete ordinary normalized private-order vocabulary in M4.
enum class PrivateOrderEventKind : std::uint8_t {
  ExchangeAcknowledged = 1,
  ExchangeRejected = 2,
  Execution = 3,
  CancelRequested = 4,
  CancelWriteOutcome = 5,
  CancellationResult = 6,
  LocalFailure = 7,
  TimeoutObserved = 8,
  DisconnectObserved = 9,
};

// ########################################################################

// ########################################################################
// Subject scope fixes whether one input targets an order, a whole account, or one private source.
enum class PrivateEventSubjectScope : std::uint8_t {
  Order = 1,
  Account = 2,
  PrivateSource = 3,
};

// ########################################################################

// ########################################################################
// Fake cancel-write outcomes describe local initiation certainty and never imply venue terminality.
enum class CancelWriteOutcome : std::uint8_t {
  DefiniteFailureBeforeAcceptance = 1,
  AcceptedAndInitiated = 2,
  AcceptedThenOutcomeLost = 3,
};

// ########################################################################

// ########################################################################
// Authoritative cancellation results distinguish terminal cancellation from a rejected cancel.
enum class CancellationResult : std::uint8_t {
  Cancelled = 1,
  CancelRejected = 2,
};

// ########################################################################

// ########################################################################
// Local failure certainty decides whether venue acceptance is impossible or remains conservative.
enum class LocalFailureCertainty : std::uint8_t {
  ProvenBeforeAcceptance = 1,
  AcceptanceCouldHaveOccurred = 2,
};

// ########################################################################

// ########################################################################
// Rejection categories are venue-neutral evidence labels rather than ownership or retry authority.
enum class ExchangeRejectionCategory : std::uint16_t {
  Unspecified = 1,
  InvalidOrder = 2,
  InsufficientAuthority = 3,
  InsufficientFunds = 4,
  PostOnlyWouldCross = 5,
  VenueRiskRejected = 6,
};

// ########################################################################

// ########################################################################
// First-admission resolution is retained separately from source input and cannot change on replay.
enum class PrivateEventResolutionKind : std::uint8_t {
  Known = 1,
  Unknown = 2,
  Conflict = 3,
  NotOrderScoped = 4,
};

// ########################################################################

// ########################################################################
// One local origin owns exactly one AEGIS event identity and both typed observation times.
struct LocalPrivateEventOrigin {
  LocalOrderEventId event_id;
  model::SourceTimestamp source_time;
  model::ReceiveTimestamp receive_time;

  // --------------------------------------------------------
  // Structural equality includes receive time for complete evidence comparison.
  friend bool operator==(const LocalPrivateEventOrigin&, const LocalPrivateEventOrigin&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One venue origin owns the complete account/source-epoch/event key and both observation times.
struct VenuePrivateEventOrigin {
  VenuePrivateEventKey event_key;
  model::SourceTimestamp source_time;
  model::ReceiveTimestamp receive_time;

  // --------------------------------------------------------
  // Structural equality includes every venue-key component and receive time.
  friend bool operator==(const VenuePrivateEventOrigin&, const VenuePrivateEventOrigin&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One reconciliation origin owns the complete epoch/cut/row key and its cut/receive times.
struct ReconciliationPrivateEventOrigin {
  recovery::ReconciliationEpochId reconciliation_epoch_id;
  AuthoritativeCutId authoritative_cut_id;
  recovery::ReconciliationRowOrdinal row_ordinal;
  model::SourceTimestamp cut_time;
  model::ReceiveTimestamp receive_time;

  // --------------------------------------------------------
  // Structural equality includes the complete row identity and receive time.
  friend bool operator==(const ReconciliationPrivateEventOrigin&,
                         const ReconciliationPrivateEventOrigin&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// The variant makes exactly one origin identity shape active in every normalized input.
using PrivateEventOriginValue = std::variant<LocalPrivateEventOrigin, VenuePrivateEventOrigin,
                                             ReconciliationPrivateEventOrigin>;

// ########################################################################

// ########################################################################
// A raw authoritative locator contains at least one syntactically valid local or exchange identity;
// the value never claims that either identity currently resolves to local ownership.
class PrivateOrderLocator final {
public:

  // --------------------------------------------------------
  // Reject the empty locator while retaining either or both exact identity values.
  [[nodiscard]] static model::Result<PrivateOrderLocator>
  create(std::optional<model::OrderId> local_order_id,
         std::optional<ExchangeOrderId> exchange_order_id);

  // --------------------------------------------------------
  [[nodiscard]] const std::optional<model::OrderId>& local_order_id() const noexcept {
    return local_order_id_;
  }

  // --------------------------------------------------------
  [[nodiscard]] const std::optional<ExchangeOrderId>& exchange_order_id() const noexcept {
    return exchange_order_id_;
  }

  // --------------------------------------------------------
  // Structural equality compares both identity presence bits and values.
  friend bool operator==(const PrivateOrderLocator&, const PrivateOrderLocator&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish only the nonempty locator shape accepted by create.
  PrivateOrderLocator(std::optional<model::OrderId> local_order_id,
                      std::optional<ExchangeOrderId> exchange_order_id) noexcept
      : local_order_id_{std::move(local_order_id)},
        exchange_order_id_{std::move(exchange_order_id)} {}

  // --------------------------------------------------------
  std::optional<model::OrderId> local_order_id_;
  std::optional<ExchangeOrderId> exchange_order_id_;
};

// ########################################################################

// ########################################################################
// Rejection detail is zero through 256 opaque bytes in inline storage; text parsing and ownership
// inference are deliberately absent.
class PrivateRejectionDetail final {
public:
  static constexpr std::size_t maximum_size = 256U;

  // --------------------------------------------------------
  // Copy a bounded opaque byte sequence, accepting an exact empty reason.
  [[nodiscard]] static model::Result<PrivateRejectionDetail>
  create(std::span<const std::byte> bytes);

  // --------------------------------------------------------
  // Borrow only the active prefix, including an empty span for absent detail.
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return std::span<const std::byte>{bytes_.data(), size_};
  }

  // --------------------------------------------------------
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  // --------------------------------------------------------
  // Equality ignores zeroed spare capacity and compares the exact active opaque bytes.
  friend bool operator==(const PrivateRejectionDetail& left,
                         const PrivateRejectionDetail& right) noexcept;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish only a completely copied bounded prefix.
  PrivateRejectionDetail(std::array<std::byte, maximum_size> bytes, std::uint16_t size) noexcept
      : bytes_{bytes}, size_{size} {}

  // --------------------------------------------------------
  std::array<std::byte, maximum_size> bytes_{};
  std::uint16_t size_{};
};

// ########################################################################

// ########################################################################
// Acknowledgement carries authoritative exchange identity and an optional raw client locator.
struct ExchangeAcknowledgedPayload {
  ExchangeOrderId exchange_order_id;
  std::optional<model::OrderId> local_order_locator;

  // --------------------------------------------------------
  friend bool operator==(const ExchangeAcknowledgedPayload&,
                         const ExchangeAcknowledgedPayload&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Rejection retains the exact raw locator, assigned category, and bounded opaque detail.
struct ExchangeRejectedPayload {
  PrivateOrderLocator locator;
  ExchangeRejectionCategory category;
  PrivateRejectionDetail detail;

  // --------------------------------------------------------
  friend bool operator==(const ExchangeRejectedPayload&, const ExchangeRejectedPayload&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Execution retains raw correlation evidence, claimed metadata, exact fill interval, price, and
// source-side presence independently from any later known/unknown correlation result.
struct ExecutionPayload {
  PrivateOrderLocator locator;
  TradeId trade_id;
  model::InstrumentId instrument_id;
  model::InstrumentMetadataRevision metadata_revision;
  model::Quantity incremental_quantity;
  model::Quantity cumulative_quantity;
  model::Price execution_price;
  std::optional<execution::OrderSide> source_side;

  // --------------------------------------------------------
  friend bool operator==(const ExecutionPayload&, const ExecutionPayload&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// A local cancel request binds one retained local order to one epoch-qualified attempt identity.
struct CancelRequestedPayload {
  model::OrderId order_id;
  CancelAttemptId cancel_attempt_id;

  // --------------------------------------------------------
  friend bool operator==(const CancelRequestedPayload&, const CancelRequestedPayload&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// A local cancel-write fact reports initiation certainty for the exact matching attempt.
struct CancelWriteOutcomePayload {
  model::OrderId order_id;
  CancelAttemptId cancel_attempt_id;
  CancelWriteOutcome outcome;

  // --------------------------------------------------------
  friend bool operator==(const CancelWriteOutcomePayload&,
                         const CancelWriteOutcomePayload&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// An authoritative cancellation carries terminal cumulative quantity exactly for Cancelled.
struct CancellationResultPayload {
  PrivateOrderLocator locator;
  CancellationResult result;
  std::optional<model::Quantity> terminal_cumulative_quantity;

  // --------------------------------------------------------
  friend bool operator==(const CancellationResultPayload&,
                         const CancellationResultPayload&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// A local failure binds the retained order and its original submission attempt to exact certainty.
struct LocalFailurePayload {
  model::OrderId order_id;
  model::SubmissionAttemptId submission_attempt_id;
  LocalFailureCertainty certainty;

  // --------------------------------------------------------
  friend bool operator==(const LocalFailurePayload&, const LocalFailurePayload&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Order timeout identifies one exact retained local order but carries no terminal authority.
struct OrderTimeoutObservedPayload {
  model::OrderId order_id;

  // --------------------------------------------------------
  friend bool operator==(const OrderTimeoutObservedPayload&,
                         const OrderTimeoutObservedPayload&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Account timeout has no payload beyond its envelope account/venue and remains nonterminal.
struct AccountTimeoutObservedPayload {

  // --------------------------------------------------------
  friend bool operator==(const AccountTimeoutObservedPayload&,
                         const AccountTimeoutObservedPayload&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Disconnect identifies the exact affected source epoch without an order locator or economics.
struct DisconnectObservedPayload {
  PrivateSourceEpochId affected_source_epoch_id;

  // --------------------------------------------------------
  friend bool operator==(const DisconnectObservedPayload&,
                         const DisconnectObservedPayload&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// The payload variant makes unrelated kind-specific fields structurally unrepresentable.
using PrivateOrderEventPayload =
    std::variant<ExchangeAcknowledgedPayload, ExchangeRejectedPayload, ExecutionPayload,
                 CancelRequestedPayload, CancelWriteOutcomePayload, CancellationResultPayload,
                 LocalFailurePayload, OrderTimeoutObservedPayload, AccountTimeoutObservedPayload,
                 DisconnectObservedPayload>;

// ########################################################################

// ########################################################################
// A validated input owns exactly one origin, one closed payload, source-normalized provenance, and
// complete structural equality; ingress equality excludes receive time only.
class NormalizedPrivateOrderInput final {
public:

  // --------------------------------------------------------
  [[nodiscard]] PrivateEventOrigin origin() const noexcept;

  // --------------------------------------------------------
  [[nodiscard]] PrivateOrderEventKind kind() const noexcept;

  // --------------------------------------------------------
  [[nodiscard]] PrivateEventSubjectScope subject_scope() const noexcept { return subject_scope_; }

  // --------------------------------------------------------
  [[nodiscard]] const model::LogicalAccountId& logical_account_id() const noexcept {
    return logical_account_id_;
  }

  // --------------------------------------------------------
  [[nodiscard]] const model::VenueId& venue_id() const noexcept { return venue_id_; }

  // --------------------------------------------------------
  [[nodiscard]] model::SourceTimestamp source_time() const noexcept;

  // --------------------------------------------------------
  [[nodiscard]] model::ReceiveTimestamp receive_time() const noexcept;

  // --------------------------------------------------------
  [[nodiscard]] const PrivateEventOriginValue& origin_value() const noexcept {
    return origin_value_;
  }

  // --------------------------------------------------------
  [[nodiscard]] const model::M4Provenance& provenance() const noexcept { return provenance_; }

  // --------------------------------------------------------
  [[nodiscard]] const PrivateOrderEventPayload& payload() const noexcept { return payload_; }

  // --------------------------------------------------------
  // Compare every field except receive time for exact event-key duplicate classification.
  [[nodiscard]] bool ingress_semantically_equal(const NormalizedPrivateOrderInput& other) const;

  // --------------------------------------------------------
  // Structural equality includes receive time for complete journal/evidence inspection.
  friend bool operator==(const NormalizedPrivateOrderInput&,
                         const NormalizedPrivateOrderInput&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Runtime construction publishes only a fully validated origin/kind/scope/provenance shape.
  NormalizedPrivateOrderInput(PrivateEventOriginValue origin_value,
                              PrivateEventSubjectScope subject_scope,
                              model::LogicalAccountId logical_account_id, model::VenueId venue_id,
                              model::M4Provenance provenance,
                              PrivateOrderEventPayload payload) noexcept
      : origin_value_{std::move(origin_value)}, subject_scope_{subject_scope},
        logical_account_id_{std::move(logical_account_id)}, venue_id_{std::move(venue_id)},
        provenance_{std::move(provenance)}, payload_{std::move(payload)} {}

  // --------------------------------------------------------
  PrivateEventOriginValue origin_value_;
  PrivateEventSubjectScope subject_scope_;
  model::LogicalAccountId logical_account_id_;
  model::VenueId venue_id_;
  model::M4Provenance provenance_;
  PrivateOrderEventPayload payload_;

  // ########################################################################
  // The composition-root factory is the only source/provenance attachment boundary.
  friend class runtime::PrivateOrderEventFactory;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::oms
