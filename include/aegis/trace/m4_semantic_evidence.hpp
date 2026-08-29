// Purpose: define storage-free M4 semantic identity, audit, callback, diagnostic, and recovery
// vocabulary before ADR-0014 assigns canonical evidence bytes.

#pragma once

#include "aegis/model/integer_input.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"
#include "aegis/oms/private_order_event.hpp"
#include "aegis/recovery/recovery_identity.hpp"

#include <compare>
#include <cstdint>
#include <limits>
#include <utility>
#include <variant>

namespace aegis::trace {

// ########################################################################
// Stable origin tags distinguish absence, ordinary source facts, reconciliation rows, and recovery
// actions without allowing one identity domain to masquerade as another.
enum class OriginatingEventIdentityKind : std::uint8_t {
  None = 0,
  Local = 1,
  Venue = 2,
  Reconciliation = 3,
  RecoveryAction = 4,
};

// ########################################################################

// ########################################################################
// Stable audit kinds name semantic row profiles while leaving their canonical bytes undefined.
enum class M4AuditKind : std::uint8_t {
  EventDisposition = 1,
  OmsTransition = 2,
  ReservationTransition = 3,
  InventoryTransition = 4,
  AccountSafetyTransition = 5,
  OrderCallbackDecision = 6,
  CallbackFault = 7,
  RecoveryGap = 8,
  RecoveryNotificationDecision = 9,
};

// ########################################################################

// ########################################################################
// Callback decisions distinguish primary-row suppression from aggregate planned and terminal rows.
enum class CallbackDecision : std::uint8_t {
  None = 0,
  Planned = 1,
  Delivered = 2,
  SuppressedDuplicate = 3,
  SuppressedBuffered = 4,
  SuppressedReplay = 5,
  Faulted = 6,
};

// ########################################################################

// ########################################################################
// Recovery gaps classify retained uncertainty without pretending callback delivery occurred.
enum class RecoveryGapKind : std::uint8_t {
  None = 0,
  PublishedNotAcknowledged = 1,
  MissingJournalInput = 2,
  InvalidSnapshot = 3,
  LocalProvenanceMissing = 4,
  CallbackDeliveryAmbiguous = 5,
};

// ########################################################################

// ########################################################################
// Diagnostic stages name the first semantic boundary that observed one bounded M4 failure.
enum class M4DiagnosticStage : std::uint8_t {
  Admission = 1,
  Shape = 2,
  Provenance = 3,
  Identity = 4,
  Correlation = 5,
  Oms = 6,
  Economics = 7,
  Evidence = 8,
  Recovery = 9,
  Callback = 10,
  Internal = 11,
};

// ########################################################################

// ########################################################################
// A diagnostic records its safety consequence separately from its domain error and processing
// stage.
enum class DiagnosticSafetyAction : std::uint8_t {
  None = 0,
  ReconciliationRequired = 1,
  Quarantined = 2,
  RuntimeFaulted = 3,
};

// ########################################################################

// ########################################################################
// A recovery action has a distinct checked one-based ordinal within one reconciliation epoch.
class RecoveryActionOrdinal final {
public:

  // --------------------------------------------------------
  // Reject zero, negative, and wider-than-u64 authored values before publishing an ordinal.
  // Interesting syntax: CheckedIntegerInput preserves signedness for checked narrowing while
  // excluding Boolean, plain/wide/Unicode character, enum, and floating-point inputs.
  template <model::detail::CheckedIntegerInput Value>
  [[nodiscard]] static model::Result<RecoveryActionOrdinal> from_value(Value value) {

    // ++++++++++++++++++++++++++++++++++++++++
    // Validate the complete authored domain before converting it to the retained width.
    if (!std::in_range<std::uint64_t>(value) || value == 0) {
      return model::Result<RecoveryActionOrdinal>::failure(model::DomainError::at_field(
          model::DomainErrorCode::InvalidRecoveryPolicy, "recovery_action_ordinal"));
    }
    return model::Result<RecoveryActionOrdinal>::success(
        RecoveryActionOrdinal{static_cast<std::uint64_t>(value)});

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Return the validated one-based ordinal without changing its reconciliation-epoch scope.
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  // --------------------------------------------------------
  // Equality and order use the complete one-based semantic value.
  friend constexpr bool operator==(RecoveryActionOrdinal, RecoveryActionOrdinal) = default;
  friend constexpr auto operator<=>(RecoveryActionOrdinal, RecoveryActionOrdinal) = default;

  // --------------------------------------------------------
  // Construction remains behind checked integral validation.
private:

  // --------------------------------------------------------
  // Retain one already validated nonzero ordinal without another failure point.
  explicit constexpr RecoveryActionOrdinal(std::uint64_t value) noexcept : value_{value} {}

  // --------------------------------------------------------
  // Store the complete one-based semantic value in its fixed-width representation.
  std::uint64_t value_;
};

// ########################################################################

// ########################################################################
// The identity-free alternative carries no sentinel identity bytes.
struct NoOriginatingEventIdentity {

  // --------------------------------------------------------
  // Equality confirms that both values intentionally carry no originating identity.
  friend bool operator==(const NoOriginatingEventIdentity&,
                         const NoOriginatingEventIdentity&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One local originating identity carries only the AEGIS-minted local event ID.
struct LocalOriginatingEventIdentity {
  oms::LocalOrderEventId event_id;

  // --------------------------------------------------------
  // Structural equality compares the complete AEGIS-minted local event identity.
  friend bool operator==(const LocalOriginatingEventIdentity&,
                         const LocalOriginatingEventIdentity&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One venue originating identity preserves the complete source-scoped event key.
struct VenueOriginatingEventIdentity {
  oms::VenuePrivateEventKey event_key;

  // --------------------------------------------------------
  // Structural equality compares the complete venue-scoped event key.
  friend bool operator==(const VenueOriginatingEventIdentity&,
                         const VenueOriginatingEventIdentity&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One reconciliation identity carries its epoch, authoritative cut, and row ordinal.
struct ReconciliationOriginatingEventIdentity {
  recovery::ReconciliationEpochId reconciliation_epoch_id;
  oms::AuthoritativeCutId authoritative_cut_id;
  recovery::ReconciliationRowOrdinal row_ordinal;

  // --------------------------------------------------------
  // Structural equality compares the complete authoritative reconciliation-row identity.
  friend bool operator==(const ReconciliationOriginatingEventIdentity&,
                         const ReconciliationOriginatingEventIdentity&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One recovery-action identity is scoped by its reconciliation epoch and independent action
// ordinal.
struct RecoveryActionOriginatingEventIdentity {
  recovery::ReconciliationEpochId reconciliation_epoch_id;
  RecoveryActionOrdinal action_ordinal;

  // --------------------------------------------------------
  // Structural equality compares the complete epoch-qualified recovery-action identity.
  friend bool operator==(const RecoveryActionOriginatingEventIdentity&,
                         const RecoveryActionOriginatingEventIdentity&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Exactly one closed originating identity alternative is active in semantic evidence.
using OriginatingEventIdentityValue =
    std::variant<NoOriginatingEventIdentity, LocalOriginatingEventIdentity,
                 VenueOriginatingEventIdentity, ReconciliationOriginatingEventIdentity,
                 RecoveryActionOriginatingEventIdentity>;

// ########################################################################

// ########################################################################
// The closed value keeps an origin tag inseparable from its exact identity-domain payload.
class OriginatingEventIdentity final {
public:

  // --------------------------------------------------------
  // Create the explicit identity-free alternative without a sentinel payload.
  [[nodiscard]] static OriginatingEventIdentity create_without_originating_event() noexcept;

  // --------------------------------------------------------
  // Create one AEGIS-local alternative from its locally minted event ID.
  [[nodiscard]] static OriginatingEventIdentity
  create_from_local_event_id(oms::LocalOrderEventId event_id) noexcept;

  // --------------------------------------------------------
  // Create one venue alternative from its complete source-scoped event key.
  [[nodiscard]] static OriginatingEventIdentity
  create_from_venue_event_key(oms::VenuePrivateEventKey event_key) noexcept;

  // --------------------------------------------------------
  // Create one authoritative reconciliation-row alternative from all three scope components.
  [[nodiscard]] static OriginatingEventIdentity
  create_from_reconciliation_row(recovery::ReconciliationEpochId reconciliation_epoch_id,
                                 oms::AuthoritativeCutId authoritative_cut_id,
                                 recovery::ReconciliationRowOrdinal row_ordinal) noexcept;

  // --------------------------------------------------------
  // Create one owner-authored recovery-action alternative from its epoch and action ordinal.
  [[nodiscard]] static OriginatingEventIdentity
  create_from_recovery_action(recovery::ReconciliationEpochId reconciliation_epoch_id,
                              RecoveryActionOrdinal action_ordinal) noexcept;

  // --------------------------------------------------------
  // Return the stable tag for the active closed identity alternative.
  [[nodiscard]] OriginatingEventIdentityKind originating_event_identity_kind() const noexcept;

  // --------------------------------------------------------
  // Borrow the exact active identity value without exposing replacement authority.
  [[nodiscard]] const OriginatingEventIdentityValue&
  originating_event_identity_value() const noexcept {
    return value_;
  }

  // --------------------------------------------------------
  // Structural equality includes the active tag and every identity component.
  friend bool operator==(const OriginatingEventIdentity&,
                         const OriginatingEventIdentity&) = default;

  // --------------------------------------------------------
  // Construction remains restricted to the tag-preserving factories.
private:

  // --------------------------------------------------------
  // Retain one already tag-correct alternative selected by a named factory.
  explicit OriginatingEventIdentity(OriginatingEventIdentityValue value) noexcept
      : value_{std::move(value)} {}

  // --------------------------------------------------------
  // Store the active origin tag and its complete payload as one immutable value.
  OriginatingEventIdentityValue value_;
};

// ########################################################################

// ########################################################################
// One checked nonempty callback range owns its first, count, and validated inclusive last ordinal.
class CallbackOrdinalRange final {
public:

  // --------------------------------------------------------
  // Validate a nonempty u32 count and reject inclusive-last wrap before publishing the range.
  template <model::detail::CheckedIntegerInput Count>
  [[nodiscard]] static model::Result<CallbackOrdinalRange>
  create_callback_ordinal_range(model::CallbackOrdinal first_callback_ordinal,
                                Count callback_count) {

    // ++++++++++++++++++++++++++++++++++++++++
    // Reject zero, negative, or wider-than-u32 counts before any narrowing conversion.
    if (!std::in_range<std::uint32_t>(callback_count) || callback_count == 0) {
      return model::Result<CallbackOrdinalRange>::failure(model::DomainError::at_field(
          model::DomainErrorCode::InvalidPrivateEvent, "callback_ordinal_range.count"));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Check the inclusive-range addition before deriving the final typed ordinal.
    const auto validated_callback_count = static_cast<std::uint32_t>(callback_count);
    const auto additional_callbacks = static_cast<std::uint64_t>(validated_callback_count - 1U);
    if (first_callback_ordinal.value() >
        std::numeric_limits<std::uint64_t>::max() - additional_callbacks) {
      return model::Result<CallbackOrdinalRange>::failure(model::DomainError::at_field(
          model::DomainErrorCode::CallbackCounterExhausted, "callback_ordinal_range.last"));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Construct the range-checked final ordinal and retain the complete range atomically.
    auto last_callback_ordinal =
        model::CallbackOrdinal::from_value(first_callback_ordinal.value() + additional_callbacks);
    if (!last_callback_ordinal) {
      return model::Result<CallbackOrdinalRange>::failure(std::move(last_callback_ordinal).error());
    }
    return model::Result<CallbackOrdinalRange>::success(
        CallbackOrdinalRange{first_callback_ordinal, validated_callback_count,
                             std::move(last_callback_ordinal).value()});

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Return the inclusive first callback ordinal in the validated range.
  [[nodiscard]] model::CallbackOrdinal first_callback_ordinal() const noexcept {
    return first_callback_ordinal_;
  }

  // --------------------------------------------------------
  // Return the exact nonzero number of callbacks in the range.
  [[nodiscard]] std::uint32_t callback_count() const noexcept { return callback_count_; }

  // --------------------------------------------------------
  // Return the inclusive final callback ordinal already proven not to wrap.
  [[nodiscard]] model::CallbackOrdinal last_callback_ordinal() const noexcept {
    return last_callback_ordinal_;
  }

  // --------------------------------------------------------
  // Structural equality pins the complete contiguous callback identity range.
  friend bool operator==(const CallbackOrdinalRange&, const CallbackOrdinalRange&) = default;

  // --------------------------------------------------------
  // Construction remains behind checked count and final-ordinal derivation.
private:

  // --------------------------------------------------------
  // Retain only values that jointly satisfy the nonempty, non-wrapping invariant.
  CallbackOrdinalRange(model::CallbackOrdinal first_callback_ordinal, std::uint32_t callback_count,
                       model::CallbackOrdinal last_callback_ordinal) noexcept
      : first_callback_ordinal_{first_callback_ordinal}, callback_count_{callback_count},
        last_callback_ordinal_{last_callback_ordinal} {}

  // --------------------------------------------------------
  // Store both inputs and the checked inclusive final ordinal without later arithmetic.
  model::CallbackOrdinal first_callback_ordinal_;
  std::uint32_t callback_count_;
  model::CallbackOrdinal last_callback_ordinal_;
};

// ########################################################################

} // namespace aegis::trace
