// Purpose: define local submission results, stable M3 stage/reason values with M4 account-safety
// reasons, and exact optional risk-limit evidence without implying exchange acknowledgement.

#pragma once

#include "aegis/model/fixed_point.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/time.hpp"
#include "aegis/risk/risk_scope.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace aegis::execution {

// ########################################################################
// Dispositions distinguish certain local rejection, local initiation, and acceptance uncertainty.
enum class SubmitDisposition : std::uint8_t {
  LocallyRejected = 1,
  WriteInitiated = 2,
  SubmissionUnknown = 3,
};

// ########################################################################
// Stages identify where a decision occurred independently from its stable reason.
enum class SubmissionStage : std::uint8_t {
  Context = 1,
  Evidence = 2,
  Route = 3,
  CanonicalValidation = 4,
  Identity = 5,
  Policy = 6,
  Risk = 7,
  Oms = 8,
  Encoding = 9,
  Initiation = 10,
  Internal = 11,
};

// ########################################################################
// Assigned reasons are persisted compatibility values; new reasons must never renumber M3 values.
enum class SubmissionReason : std::uint16_t {
  None = 0,

  ContextInactive = 1,
  WrongOwner = 2,
  SubmissionReentry = 3,
  EvidenceCapacityExceeded = 4,
  SubmissionAttemptExhausted = 5,
  SubmissionCapabilityUnavailable = 6,

  RouteNotFound = 10,
  RouteNotOwned = 11,
  RouteDisabled = 12,
  RouteInstrumentMismatch = 13,

  UnsupportedSide = 20,
  UnsupportedOrderType = 21,
  UnsupportedTimeInForce = 22,
  PriceNotPositive = 23,
  PriceScaleExceeded = 24,
  PriceTickMismatch = 25,
  QuantityNotPositive = 26,
  QuantityScaleExceeded = 27,
  QuantityBelowMinimum = 28,
  QuantityStepMismatch = 29,
  UnsupportedContractEconomics = 30,

  OrderIdentityExhausted = 40,

  RiskArithmeticFailure = 50,
  SingleOrderQuantityExceeded = 51,
  SingleOrderNotionalExceeded = 52,
  OpenOrderCountExceeded = 53,
  GrossReservedNotionalExceeded = 54,
  WorstCasePositionQuantityExceeded = 55,
  WorstCasePositionNotionalExceeded = 56,
  ReservationCapacityExceeded = 57,
  AccountReconciliationRequired = 58,
  AccountQuarantined = 59,

  DuplicateOrderIdentity = 70,
  OmsCapacityExceeded = 71,

  EncodingFailed = 80,

  InitiationDefinitelyFailed = 90,
  InitiationOutcomeUnknown = 91,

  SubmissionRuntimeFaulted = 100,
};

// ########################################################################
// Risk evidence states which nominal representation the observed and configured values use.
enum class RiskMeasureKind : std::uint8_t {
  None = 0,
  Quantity = 1,
  QuoteNotional = 2,
  OrderCount = 3,
};

// ########################################################################
// One limit rejection owns exactly one scope and one pair of same-domain observed/limit values.
class RiskLimitEvidence final {
public:

  // --------------------------------------------------------
  // Construct exact quantity evidence without exposing a generic decimal-unit escape hatch.
  [[nodiscard]] static RiskLimitEvidence create_quantity_evidence(risk::RiskScopeKind scope,
                                                                  model::Quantity observed,
                                                                  model::Quantity limit) noexcept {
    return RiskLimitEvidence{scope,        RiskMeasureKind::Quantity,
                             observed,     limit,
                             std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt};
  }

  // --------------------------------------------------------
  // Construct exact quote-notional evidence in the policy's declared currency and scale.
  [[nodiscard]] static RiskLimitEvidence
  create_quote_notional_evidence(risk::RiskScopeKind scope, model::Notional observed,
                                 model::Notional limit) noexcept {
    return RiskLimitEvidence{scope,        RiskMeasureKind::QuoteNotional,
                             std::nullopt, std::nullopt,
                             observed,     limit,
                             std::nullopt, std::nullopt};
  }

  // --------------------------------------------------------
  // Construct exact unsigned order-count evidence.
  [[nodiscard]] static RiskLimitEvidence create_order_count_evidence(risk::RiskScopeKind scope,
                                                                     std::uint64_t observed,
                                                                     std::uint64_t limit) noexcept {
    return RiskLimitEvidence{scope,        RiskMeasureKind::OrderCount,
                             std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt,
                             observed,     limit};
  }

  // --------------------------------------------------------
  // Return the risk scope whose limit produced this evidence.
  [[nodiscard]] risk::RiskScopeKind scope() const noexcept { return scope_; }

  // --------------------------------------------------------
  // Return which quantity, notional, or count measure was compared.
  [[nodiscard]] RiskMeasureKind measure_kind() const noexcept { return measure_kind_; }

  // --------------------------------------------------------
  // Return the observed quantity only for quantity evidence.
  [[nodiscard]] const std::optional<model::Quantity>& observed_quantity() const noexcept {
    return observed_quantity_;
  }

  // --------------------------------------------------------
  // Return the configured quantity limit only for quantity evidence.
  [[nodiscard]] const std::optional<model::Quantity>& quantity_limit() const noexcept {
    return quantity_limit_;
  }

  // --------------------------------------------------------
  // Return the observed quote notional only for notional evidence.
  [[nodiscard]] const std::optional<model::Notional>& observed_notional() const noexcept {
    return observed_notional_;
  }

  // --------------------------------------------------------
  // Return the configured quote-notional limit only for notional evidence.
  [[nodiscard]] const std::optional<model::Notional>& notional_limit() const noexcept {
    return notional_limit_;
  }

  // --------------------------------------------------------
  // Return the observed open-order count only for count evidence.
  [[nodiscard]] const std::optional<std::uint64_t>& observed_count() const noexcept {
    return observed_count_;
  }

  // --------------------------------------------------------
  // Return the configured open-order limit only for count evidence.
  [[nodiscard]] const std::optional<std::uint64_t>& count_limit() const noexcept {
    return count_limit_;
  }

  // --------------------------------------------------------
  // Structural equality makes stable rejection evidence directly testable.
  friend bool operator==(const RiskLimitEvidence&, const RiskLimitEvidence&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Private construction makes the three factories the only valid field-shape authority.
  RiskLimitEvidence(risk::RiskScopeKind scope, RiskMeasureKind measure_kind,
                    std::optional<model::Quantity> observed_quantity,
                    std::optional<model::Quantity> quantity_limit,
                    std::optional<model::Notional> observed_notional,
                    std::optional<model::Notional> notional_limit,
                    std::optional<std::uint64_t> observed_count,
                    std::optional<std::uint64_t> count_limit) noexcept
      : scope_{scope}, measure_kind_{measure_kind},
        observed_quantity_{std::move(observed_quantity)},
        quantity_limit_{std::move(quantity_limit)},
        observed_notional_{std::move(observed_notional)},
        notional_limit_{std::move(notional_limit)}, observed_count_{observed_count},
        count_limit_{count_limit} {}

  // --------------------------------------------------------
  risk::RiskScopeKind scope_;
  RiskMeasureKind measure_kind_;
  std::optional<model::Quantity> observed_quantity_;
  std::optional<model::Quantity> quantity_limit_;
  std::optional<model::Notional> observed_notional_;
  std::optional<model::Notional> notional_limit_;
  std::optional<std::uint64_t> observed_count_;
  std::optional<std::uint64_t> count_limit_;
};

// ########################################################################
// SubmitResult is a synchronous local decision only. It deliberately has no acknowledged,
// exchange-accepted, exchange-order-ID, fill, or retry field.
class SubmitResult final {
public:

  // --------------------------------------------------------
  // Construct a definite local rejection with only the identities already consumed by its stage.
  [[nodiscard]] static SubmitResult create_locally_rejected_result(
      SubmissionStage stage, SubmissionReason reason,
      std::optional<model::SubmissionAttemptId> attempt_id = std::nullopt,
      std::optional<model::OrderId> order_id = std::nullopt,
      std::optional<RiskLimitEvidence> risk_evidence = std::nullopt,
      std::optional<std::uint64_t> local_path_nanoseconds = std::nullopt) noexcept {
    return SubmitResult{SubmitDisposition::LocallyRejected,
                        stage,
                        reason,
                        attempt_id,
                        std::move(order_id),
                        std::move(risk_evidence),
                        local_path_nanoseconds};
  }

  // --------------------------------------------------------
  // Report successful local fake initiation without implying any exchange response.
  [[nodiscard]] static SubmitResult create_write_initiated_result(
      model::SubmissionAttemptId attempt_id, model::OrderId order_id,
      std::optional<std::uint64_t> local_path_nanoseconds = std::nullopt) noexcept {
    return SubmitResult{SubmitDisposition::WriteInitiated,
                        SubmissionStage::Initiation,
                        SubmissionReason::None,
                        attempt_id,
                        std::move(order_id),
                        std::nullopt,
                        local_path_nanoseconds};
  }

  // --------------------------------------------------------
  // Report that fake acceptance may have occurred and conservative exposure must remain held.
  [[nodiscard]] static SubmitResult create_submission_unknown_result(
      model::SubmissionAttemptId attempt_id, model::OrderId order_id,
      std::optional<std::uint64_t> local_path_nanoseconds = std::nullopt) noexcept {
    return SubmitResult{SubmitDisposition::SubmissionUnknown,
                        SubmissionStage::Initiation,
                        SubmissionReason::InitiationOutcomeUnknown,
                        attempt_id,
                        std::move(order_id),
                        std::nullopt,
                        local_path_nanoseconds};
  }

  // --------------------------------------------------------
  // Return whether the submission was locally rejected, initiated, or left unknown.
  [[nodiscard]] SubmitDisposition disposition() const noexcept { return disposition_; }

  // --------------------------------------------------------
  // Return the furthest submission stage represented by this terminal result.
  [[nodiscard]] SubmissionStage stage() const noexcept { return stage_; }

  // --------------------------------------------------------
  // Return the stable reason associated with the disposition and stage.
  [[nodiscard]] SubmissionReason reason() const noexcept { return reason_; }

  // --------------------------------------------------------
  // Return the attempt identity only after submission admission assigned one.
  [[nodiscard]] const std::optional<model::SubmissionAttemptId>& attempt_id() const noexcept {
    return attempt_id_;
  }

  // --------------------------------------------------------
  // Return the local order identity only after OMS admission assigned one.
  [[nodiscard]] const std::optional<model::OrderId>& order_id() const noexcept { return order_id_; }

  // --------------------------------------------------------
  // Return exact limit evidence only for risk-policy rejection.
  [[nodiscard]] const std::optional<RiskLimitEvidence>& risk_evidence() const noexcept {
    return risk_evidence_;
  }

  // --------------------------------------------------------
  // Return measured local-path duration when both endpoint readings were available.
  [[nodiscard]] const std::optional<std::uint64_t>& local_path_nanoseconds() const noexcept {
    return local_path_nanoseconds_;
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Closed factories are the only constructors, preventing an acknowledged-style disposition.
  SubmitResult(SubmitDisposition disposition, SubmissionStage stage, SubmissionReason reason,
               std::optional<model::SubmissionAttemptId> attempt_id,
               std::optional<model::OrderId> order_id,
               std::optional<RiskLimitEvidence> risk_evidence,
               std::optional<std::uint64_t> local_path_nanoseconds) noexcept
      : disposition_{disposition}, stage_{stage}, reason_{reason}, attempt_id_{attempt_id},
        order_id_{std::move(order_id)}, risk_evidence_{std::move(risk_evidence)},
        local_path_nanoseconds_{local_path_nanoseconds} {}

  // --------------------------------------------------------
  SubmitDisposition disposition_;
  SubmissionStage stage_;
  SubmissionReason reason_;
  std::optional<model::SubmissionAttemptId> attempt_id_;
  std::optional<model::OrderId> order_id_;
  std::optional<RiskLimitEvidence> risk_evidence_;
  std::optional<std::uint64_t> local_path_nanoseconds_;
};

// ########################################################################

} // namespace aegis::execution
