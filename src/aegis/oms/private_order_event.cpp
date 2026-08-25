// Purpose: validate bounded private-event primitives and project complete receive-time-free
// ingress semantics and normalized equality without correlation, OMS, or economic mutation.

#include "aegis/oms/private_order_event.hpp"

#include <algorithm>
#include <cstddef>
#include <type_traits>

namespace aegis::oms {
namespace {

// --------------------------------------------------------
// Compare exactly one active origin shape while deliberately excluding receive time.
[[nodiscard]] bool are_ingress_origins_equal(const PrivateEventOriginValue& left,
                                             const PrivateEventOriginValue& right) noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Different active origin alternatives can never identify the same ingress fact.
  if (left.index() != right.index()) {
    return false;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Compare every identity and source-time field in the matching alternative.
  return std::visit(
      [&right](const auto& left_origin) {
        using Origin = std::decay_t<decltype(left_origin)>;
        const auto& right_origin = std::get<Origin>(right);
        if constexpr (std::is_same_v<Origin, LocalPrivateEventOrigin>) {
          return left_origin.event_id == right_origin.event_id &&
                 left_origin.source_time == right_origin.source_time;
        } else if constexpr (std::is_same_v<Origin, VenuePrivateEventOrigin>) {
          return left_origin.event_key == right_origin.event_key &&
                 left_origin.source_time == right_origin.source_time;
        } else {
          return left_origin.reconciliation_epoch_id == right_origin.reconciliation_epoch_id &&
                 left_origin.authoritative_cut_id == right_origin.authoritative_cut_id &&
                 left_origin.row_ordinal == right_origin.row_ordinal &&
                 left_origin.cut_time == right_origin.cut_time;
        }
      },
      left);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Reject the empty locator while retaining either or both exact identity values.
model::Result<PrivateOrderLocator>
PrivateOrderLocator::create(std::optional<model::OrderId> local_order_id,
                            std::optional<ExchangeOrderId> exchange_order_id) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Absence of both identities cannot correlate or retain an authoritative unknown locator.
  if (!local_order_id.has_value() && !exchange_order_id.has_value()) {
    return model::Result<PrivateOrderLocator>::failure(model::DomainError::at_field(
        model::DomainErrorCode::InvalidPrivateEvent, "private_event.order_locator"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the exact nonempty presence shape without interpreting opaque exchange bytes.
  return model::Result<PrivateOrderLocator>::success(
      PrivateOrderLocator{std::move(local_order_id), std::move(exchange_order_id)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Copy a bounded opaque byte sequence, accepting an exact empty reason.
model::Result<PrivateRejectionDetail>
PrivateRejectionDetail::create(std::span<const std::byte> bytes) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject over-bound evidence before copying any caller bytes.
  if (bytes.size() > maximum_size) {
    return model::Result<PrivateRejectionDetail>::failure(model::DomainError::at_field(
        model::DomainErrorCode::InvalidPrivateEvent, "private_event.rejection_detail"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy the active prefix into zero-initialized inline storage for deterministic spare bytes.
  std::array<std::byte, maximum_size> storage{};
  std::copy(bytes.begin(), bytes.end(), storage.begin());
  return model::Result<PrivateRejectionDetail>::success(
      PrivateRejectionDetail{storage, static_cast<std::uint16_t>(bytes.size())});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Equality ignores zeroed spare capacity and compares the exact active opaque bytes.
bool operator==(const PrivateRejectionDetail& left, const PrivateRejectionDetail& right) noexcept {
  return left.size_ == right.size_ &&
         std::equal(left.bytes_.begin(),
                    left.bytes_.begin() + static_cast<std::ptrdiff_t>(left.size_),
                    right.bytes_.begin());
}

// --------------------------------------------------------
// Return the assigned origin kind from the active typed alternative.
PrivateEventOrigin NormalizedPrivateOrderInput::origin() const noexcept {
  switch (origin_value_.index()) {
  case 0U:
    return PrivateEventOrigin::Local;
  case 1U:
    return PrivateEventOrigin::Venue;
  default:
    return PrivateEventOrigin::Reconciliation;
  }
}

// --------------------------------------------------------
// Return the assigned event kind from the active closed payload alternative.
PrivateOrderEventKind NormalizedPrivateOrderInput::kind() const noexcept {
  switch (payload_.index()) {
  case 0U:
    return PrivateOrderEventKind::ExchangeAcknowledged;
  case 1U:
    return PrivateOrderEventKind::ExchangeRejected;
  case 2U:
    return PrivateOrderEventKind::Execution;
  case 3U:
    return PrivateOrderEventKind::CancelRequested;
  case 4U:
    return PrivateOrderEventKind::CancelWriteOutcome;
  case 5U:
    return PrivateOrderEventKind::CancellationResult;
  case 6U:
    return PrivateOrderEventKind::LocalFailure;
  case 7U:
  case 8U:
    return PrivateOrderEventKind::TimeoutObserved;
  default:
    return PrivateOrderEventKind::DisconnectObserved;
  }
}

// --------------------------------------------------------
// Return source time from the active origin without confusing it with receive time.
model::SourceTimestamp NormalizedPrivateOrderInput::source_time() const noexcept {
  return std::visit(
      [](const auto& origin_value) -> model::SourceTimestamp {
        using Origin = std::decay_t<decltype(origin_value)>;
        if constexpr (std::is_same_v<Origin, ReconciliationPrivateEventOrigin>) {
          return origin_value.cut_time;
        } else {
          return origin_value.source_time;
        }
      },
      origin_value_);
}

// --------------------------------------------------------
// Return receive time from the active origin for complete evidence and latency observation.
model::ReceiveTimestamp NormalizedPrivateOrderInput::receive_time() const noexcept {
  return std::visit([](const auto& origin_value) { return origin_value.receive_time; },
                    origin_value_);
}

// --------------------------------------------------------
// Compare every correlation-independent ingress field except receive time.
bool NormalizedPrivateOrderInput::is_ingress_semantically_equal_to(
    const NormalizedPrivateOrderInput& other) const {
  return subject_scope_ == other.subject_scope_ &&
         logical_account_id_ == other.logical_account_id_ && venue_id_ == other.venue_id_ &&
         provenance_ == other.provenance_ && payload_ == other.payload_ &&
         are_ingress_origins_equal(origin_value_, other.origin_value_);
}

// --------------------------------------------------------
// Project every normalized source field except its local receive observation.
PrivateEventIngressSemanticValue
PrivateEventIngressSemanticValue::from_normalized_input(const NormalizedPrivateOrderInput& input) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Remove only receive time while preserving the exact active source-domain identity.
  auto ingress_origin = std::visit(
      [](const auto& origin) -> PrivateIngressOriginValue {
        using Origin = std::decay_t<decltype(origin)>;
        if constexpr (std::is_same_v<Origin, LocalPrivateEventOrigin>) {
          return LocalPrivateIngressOrigin{origin.event_id, origin.source_time};
        } else if constexpr (std::is_same_v<Origin, VenuePrivateEventOrigin>) {
          return VenuePrivateIngressOrigin{origin.event_key, origin.source_time};
        } else {
          static_assert(std::is_same_v<Origin, ReconciliationPrivateEventOrigin>);
          return ReconciliationPrivateIngressOrigin{origin.reconciliation_epoch_id,
                                                    origin.authoritative_cut_id, origin.row_ordinal,
                                                    origin.cut_time};
        }
      },
      input.origin_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy the remaining immutable source fields without enriching provenance or correlation.
  return PrivateEventIngressSemanticValue{std::move(ingress_origin),  input.subject_scope(),
                                          input.logical_account_id(), input.venue_id(),
                                          input.provenance(),         input.payload()};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::oms
