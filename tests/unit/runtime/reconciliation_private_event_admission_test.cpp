// Purpose: independently prove isolated reconciliation-event capacity, shared executor ordering,
// loss-fence interaction, owner evidence, and move-only capability lifetime.

#include "aegis/runtime/dedicated_executor_driver.hpp"
#include "aegis/runtime/private_order_admission.hpp"
#include "aegis/runtime/serialized_executor.hpp"
#include "m4_private_event_fixture.hpp"
#include "m4_test_authority.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using namespace aegis;

// ########################################################################
// These compile-time probes distinguish the sole stable-lvalue ingress contract from both deleted
// temporary paths without invoking either forbidden overload.
template <typename Executor, typename Attempt>
concept AdmitsReconciliationConstReference = requires(Executor& executor, const Attempt& attempt) {
  {
    executor.try_admit_reconciliation_event(attempt)
  } -> std::same_as<model::Result<runtime::ReconciliationAdmissionDecision>>;
};

template <typename Executor, typename Attempt>
concept AdmitsReconciliationMutableRvalue = requires(Executor& executor, Attempt attempt) {
  executor.try_admit_reconciliation_event(std::move(attempt));
};

template <typename Executor, typename Attempt>
concept AdmitsReconciliationConstRvalue = requires(Executor& executor, const Attempt attempt) {
  executor.try_admit_reconciliation_event(std::move(attempt));
};

// ########################################################################
// Authored type expectations pin the nominal reconciliation boundary independently from ordinary
// private admission and from implementation declaration order.
static_assert(static_cast<std::uint8_t>(runtime::TurnKind::ReconciliationCommand) == 6U);
static_assert(!std::default_initializable<runtime::AdmittedReconciliationEventSlot>);
static_assert(!std::copy_constructible<runtime::AdmittedReconciliationEventSlot>);
static_assert(!std::is_copy_assignable_v<runtime::AdmittedReconciliationEventSlot>);
static_assert(std::move_constructible<runtime::AdmittedReconciliationEventSlot>);
static_assert(!std::is_move_assignable_v<runtime::AdmittedReconciliationEventSlot>);
static_assert(!std::default_initializable<runtime::AdmittedReconciliationEventSlotView>);
static_assert(!std::constructible_from<runtime::AdmittedReconciliationEventSlot,
                                       runtime::AdmittedPrivateOrderSlot>);
static_assert(!std::constructible_from<runtime::AdmittedPrivateOrderSlot,
                                       runtime::AdmittedReconciliationEventSlot>);
static_assert(AdmitsReconciliationConstReference<runtime::SerializedExecutor,
                                                 oms::ReconciliationPrivateEventIngressAttempt>);
static_assert(!AdmitsReconciliationMutableRvalue<runtime::SerializedExecutor,
                                                 oms::ReconciliationPrivateEventIngressAttempt>);
static_assert(!AdmitsReconciliationConstRvalue<runtime::SerializedExecutor,
                                               oms::ReconciliationPrivateEventIngressAttempt>);
static_assert(!noexcept(
    std::declval<const runtime::SerializedExecutor&>().reconciliation_admission_observation(
        std::declval<model::AdmissionOrdinal>())));

// ########################################################################
// This bounded test owner records ordinary and reconciliation evidence in separate namespaces,
// retains no source references, and may keep exactly one moved reconciliation token for lifetime
// checks. A mutex makes both evidence oracles linearizable with their matching publication.
class ReconciliationAdmissionOwner final : public runtime::PrivateAdmissionOwner {
public:
  static constexpr std::size_t history_capacity = 16U;

  // ########################################################################
  // Closed reconciliation scripts distinguish ordinary consumption, a mismatched disposition, and
  // retained-account completions with either matching or deliberately absent committed evidence.
  enum class ReconciliationCompletionMode : std::uint8_t {
    ConsumeApplied = 1,
    ConsumeWithMismatchedDisposition = 2,
    RetainAccountWithCommittedEvidence = 3,
    RetainAccountWithoutCommittedEvidence = 4,
  };

  // ########################################################################

  // --------------------------------------------------------
  // Select one reconciliation completion script and the account reason returned by retained modes.
  void select_reconciliation_completion(ReconciliationCompletionMode mode,
                                        risk::AccountSafetyReason retention_reason =
                                            risk::AccountSafetyReason::TimeoutObserved) noexcept {
    reconciliation_completion_mode_ = mode;
    reconciliation_retention_reason_ = retention_reason;
  }

  // --------------------------------------------------------
  // Inspect and consume one ordinary-private turn while publishing only ordinary evidence.
  [[nodiscard]] runtime::PrivateTurnCompletion
  commit_private_order_turn(runtime::AdmittedPrivateOrderSlot admitted) noexcept override {
    auto inspected = admitted.inspect_admitted_private_order_slot();
    if (!inspected) {
      return runtime::RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
          inspected.error());
    }
    if (ordinary_view_count_ == history_capacity || callback_count_ == history_capacity) {
      return runtime::RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
          create_test_retention_error());
    }
    ordinary_views_[ordinary_view_count_].emplace(std::move(inspected).value());
    const auto attempt_ordinal =
        ordinary_views_[ordinary_view_count_]->admission_receipt().attempt_ordinal;
    ++ordinary_view_count_;
    callback_kinds_[callback_count_].emplace(runtime::TurnKind::PrivateCommand);
    ++callback_count_;
    publish_ordinary_disposition(attempt_ordinal);
    return runtime::ConsumedPrivateTurn{oms::PrivateEventDisposition::Applied};
  }

  // --------------------------------------------------------
  // Inspect and consume one reconciliation turn, optionally retaining its sole moved capability,
  // while publishing evidence only in the reconciliation namespace.
  [[nodiscard]] runtime::PrivateTurnCompletion commit_reconciliation_event_turn(
      runtime::AdmittedReconciliationEventSlot admitted) noexcept override {
    auto inspected = admitted.inspect_admitted_reconciliation_event_slot();
    if (!inspected) {
      return runtime::RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
          inspected.error());
    }
    if (reconciliation_view_count_ == history_capacity || callback_count_ == history_capacity) {
      return runtime::RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
          create_test_retention_error());
    }
    reconciliation_views_[reconciliation_view_count_].emplace(std::move(inspected).value());
    const auto attempt_ordinal =
        reconciliation_views_[reconciliation_view_count_]->admission_receipt().attempt_ordinal;
    ++reconciliation_view_count_;
    callback_kinds_[callback_count_].emplace(runtime::TurnKind::ReconciliationCommand);
    ++callback_count_;
    if (should_retain_reconciliation_token_) {
      retained_reconciliation_token_.emplace(std::move(admitted));
    }
    switch (reconciliation_completion_mode_) {
    case ReconciliationCompletionMode::ConsumeApplied:
      publish_reconciliation_disposition(attempt_ordinal, oms::PrivateEventDisposition::Applied);
      signal_reconciliation_completion_if_requested();
      return runtime::ConsumedPrivateTurn{oms::PrivateEventDisposition::Applied};
    case ReconciliationCompletionMode::ConsumeWithMismatchedDisposition:
      publish_reconciliation_disposition(attempt_ordinal,
                                         oms::PrivateEventDisposition::ProjectionOnly);
      signal_reconciliation_completion_if_requested();
      return runtime::ConsumedPrivateTurn{oms::PrivateEventDisposition::Applied};
    case ReconciliationCompletionMode::RetainAccountWithCommittedEvidence: {
      auto error = create_reconciliation_retention_error();
      publish_reconciliation_retention_error(attempt_ordinal, error);
      signal_reconciliation_completion_if_requested();
      return runtime::RetainedPrivateTurn::create_retained_private_turn_for_account(
          std::move(error), reconciliation_retention_reason_);
    }
    case ReconciliationCompletionMode::RetainAccountWithoutCommittedEvidence:
      signal_reconciliation_completion_if_requested();
      return runtime::RetainedPrivateTurn::create_retained_private_turn_for_account(
          create_reconciliation_retention_error(), reconciliation_retention_reason_);
    }
    return runtime::RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
        create_test_retention_error());
  }

  // --------------------------------------------------------
  // Find only a terminal ordinary disposition published for the exact shared admission ordinal.
  [[nodiscard]] std::optional<oms::PrivateEventDisposition>
  find_committed_private_event_disposition(
      model::AdmissionOrdinal attempt_ordinal) const noexcept override {
    std::lock_guard lock{evidence_mutex_};
    for (std::size_t index = 0U; index < ordinary_evidence_count_; ++index) {
      if (ordinary_evidence_ordinals_[index] == attempt_ordinal) {
        return oms::PrivateEventDisposition::Applied;
      }
    }
    return std::nullopt;
  }

  // --------------------------------------------------------
  // Find only a terminal reconciliation disposition published for the exact shared ordinal.
  [[nodiscard]] std::optional<oms::PrivateEventDisposition>
  find_committed_reconciliation_event_disposition(
      model::AdmissionOrdinal attempt_ordinal) const noexcept override {
    std::lock_guard lock{evidence_mutex_};
    for (std::size_t index = 0U; index < reconciliation_evidence_count_; ++index) {
      if (reconciliation_evidence_ordinals_[index] == attempt_ordinal) {
        return reconciliation_evidence_dispositions_[index];
      }
    }
    return std::nullopt;
  }

  // --------------------------------------------------------
  // This owner never retains an ordinary event, so no ordinary ordinal has a retention error.
  [[nodiscard]] const model::DomainError*
  find_committed_retained_private_event_error(model::AdmissionOrdinal) const noexcept override {
    return nullptr;
  }

  // --------------------------------------------------------
  // Find one stable reconciliation retention error without consulting the ordinary namespace.
  [[nodiscard]] const model::DomainError* find_committed_retained_reconciliation_event_error(
      model::AdmissionOrdinal attempt_ordinal) const noexcept override {
    std::lock_guard lock{evidence_mutex_};
    for (std::size_t index = 0U; index < reconciliation_retention_evidence_count_; ++index) {
      if (reconciliation_retention_evidence_ordinals_[index] == attempt_ordinal) {
        return &reconciliation_retention_evidence_[index].value();
      }
    }
    return nullptr;
  }

  // --------------------------------------------------------
  // Copy each configured-account fence and its context before reporting successful application.
  [[nodiscard]] model::Result<void>
  apply_account_safety_fence(const runtime::AccountSafetyFenceTurn& fence,
                             const runtime::ControlTurnContext& context) noexcept override {
    if (account_fence_count_ == history_capacity || callback_count_ == history_capacity) {
      return model::Result<void>::create_failure(create_test_retention_error());
    }
    account_fences_[account_fence_count_].emplace(fence);
    account_fence_contexts_[account_fence_count_].emplace(context);
    ++account_fence_count_;
    callback_kinds_[callback_count_].emplace(runtime::TurnKind::AccountSafetyFence);
    ++callback_count_;
    return model::Result<void>::create_success();
  }

  // --------------------------------------------------------
  // Copy the permanent reasonless global fence and its context before reporting owner application.
  [[nodiscard]] model::Result<void>
  apply_global_private_fence(const runtime::GlobalPrivateFenceTurn& fence,
                             const runtime::ControlTurnContext& context) noexcept override {
    global_fence_.emplace(fence);
    global_fence_context_.emplace(context);
    if (callback_count_ == history_capacity) {
      return model::Result<void>::create_failure(create_test_retention_error());
    }
    callback_kinds_[callback_count_].emplace(runtime::TurnKind::GlobalPrivateFence);
    ++callback_count_;
    return model::Result<void>::create_success();
  }

  // --------------------------------------------------------
  // Retain the next reconciliation token only after successfully inspecting it in its owner turn.
  void retain_next_reconciliation_token() noexcept { should_retain_reconciliation_token_ = true; }

  // --------------------------------------------------------
  // Signal one test-owned latch after reconciliation evidence has been published in the callback.
  void signal_reconciliation_completion(std::latch& completion_latch) noexcept {
    reconciliation_completion_latch_ = &completion_latch;
  }

  // --------------------------------------------------------
  // Borrow the token intentionally retained beyond its active owner-turn lifetime.
  [[nodiscard]] const std::optional<runtime::AdmittedReconciliationEventSlot>&
  retained_reconciliation_token() const noexcept {
    return retained_reconciliation_token_;
  }

  // --------------------------------------------------------
  // Return the number of successfully inspected ordinary-private owner turns.
  [[nodiscard]] std::size_t ordinary_view_count() const noexcept { return ordinary_view_count_; }

  // --------------------------------------------------------
  // Borrow one caller-indexed ordinary-private view retained in bounded callback order.
  [[nodiscard]] const runtime::AdmittedPrivateOrderSlotView&
  ordinary_view_at(std::size_t index) const noexcept {
    return ordinary_views_[index].value();
  }

  // --------------------------------------------------------
  // Return the number of successfully inspected reconciliation owner turns.
  [[nodiscard]] std::size_t reconciliation_view_count() const noexcept {
    return reconciliation_view_count_;
  }

  // --------------------------------------------------------
  // Borrow one caller-indexed reconciliation view retained in bounded callback order.
  [[nodiscard]] const runtime::AdmittedReconciliationEventSlotView&
  reconciliation_view_at(std::size_t index) const noexcept {
    return reconciliation_views_[index].value();
  }

  // --------------------------------------------------------
  // Return the number of owner callbacks retained across command and fence kinds.
  [[nodiscard]] std::size_t callback_count() const noexcept { return callback_count_; }

  // --------------------------------------------------------
  // Return one caller-indexed callback kind from the shared owner progression history.
  [[nodiscard]] runtime::TurnKind callback_kind_at(std::size_t index) const noexcept {
    return callback_kinds_[index].value();
  }

  // --------------------------------------------------------
  // Return the number of configured-account fence intervals successfully delivered to the owner.
  [[nodiscard]] std::size_t account_fence_count() const noexcept { return account_fence_count_; }

  // --------------------------------------------------------
  // Borrow one caller-indexed configured-account fence from bounded delivery history.
  [[nodiscard]] const runtime::AccountSafetyFenceTurn&
  account_fence_at(std::size_t index) const noexcept {
    return account_fences_[index].value();
  }

  // --------------------------------------------------------
  // Borrow the control context paired with one caller-indexed configured-account fence.
  [[nodiscard]] const runtime::ControlTurnContext&
  account_fence_context_at(std::size_t index) const noexcept {
    return account_fence_contexts_[index].value();
  }

  // --------------------------------------------------------
  // Borrow the delivered reasonless global fence, when one has been owner-applied.
  [[nodiscard]] const std::optional<runtime::GlobalPrivateFenceTurn>&
  global_fence() const noexcept {
    return global_fence_;
  }

  // --------------------------------------------------------
  // Borrow the owner context paired with the reasonless global fence.
  [[nodiscard]] const std::optional<runtime::ControlTurnContext>&
  global_fence_context() const noexcept {
    return global_fence_context_;
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish one ordinary disposition before the corresponding consumed completion returns.
  void publish_ordinary_disposition(model::AdmissionOrdinal attempt_ordinal) noexcept {
    std::lock_guard lock{evidence_mutex_};
    if (ordinary_evidence_count_ == history_capacity) {
      return;
    }
    ordinary_evidence_ordinals_[ordinary_evidence_count_].emplace(attempt_ordinal);
    ++ordinary_evidence_count_;
  }

  // --------------------------------------------------------
  // Publish one reconciliation disposition in its separate oracle namespace before completion.
  void publish_reconciliation_disposition(model::AdmissionOrdinal attempt_ordinal,
                                          oms::PrivateEventDisposition disposition) noexcept {
    std::lock_guard lock{evidence_mutex_};
    if (reconciliation_evidence_count_ == history_capacity) {
      return;
    }
    reconciliation_evidence_ordinals_[reconciliation_evidence_count_].emplace(attempt_ordinal);
    reconciliation_evidence_dispositions_[reconciliation_evidence_count_].emplace(disposition);
    ++reconciliation_evidence_count_;
  }

  // --------------------------------------------------------
  // Publish one immutable retained error in fixed storage before its completion leaves the owner.
  void publish_reconciliation_retention_error(model::AdmissionOrdinal attempt_ordinal,
                                              const model::DomainError& error) noexcept {
    std::lock_guard lock{evidence_mutex_};
    if (reconciliation_retention_evidence_count_ == history_capacity) {
      return;
    }
    reconciliation_retention_evidence_ordinals_[reconciliation_retention_evidence_count_].emplace(
        attempt_ordinal);
    reconciliation_retention_evidence_[reconciliation_retention_evidence_count_].emplace(error);
    ++reconciliation_retention_evidence_count_;
  }

  // --------------------------------------------------------
  // Release a dedicated-driver waiter only after this owner has published its selected evidence.
  void signal_reconciliation_completion_if_requested() noexcept {
    if (reconciliation_completion_latch_ != nullptr) {
      reconciliation_completion_latch_->count_down();
    }
  }

  // --------------------------------------------------------
  // Create one stable authored error for unreachable bounded test-owner overflow branches.
  [[nodiscard]] static model::DomainError create_test_retention_error() {
    return model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                               "test_reconciliation_admission_owner");
  }

  // --------------------------------------------------------
  // Create the stable owner-authored error used by valid and invalid retained completion scripts.
  [[nodiscard]] static model::DomainError create_reconciliation_retention_error() {
    return model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                               "test_reconciliation_retention");
  }

  // --------------------------------------------------------
  std::array<std::optional<runtime::AdmittedPrivateOrderSlotView>, history_capacity>
      ordinary_views_{};
  std::array<std::optional<runtime::AdmittedReconciliationEventSlotView>, history_capacity>
      reconciliation_views_{};
  std::array<std::optional<model::AdmissionOrdinal>, history_capacity>
      ordinary_evidence_ordinals_{};
  std::array<std::optional<model::AdmissionOrdinal>, history_capacity>
      reconciliation_evidence_ordinals_{};
  std::array<std::optional<oms::PrivateEventDisposition>, history_capacity>
      reconciliation_evidence_dispositions_{};
  std::array<std::optional<model::AdmissionOrdinal>, history_capacity>
      reconciliation_retention_evidence_ordinals_{};
  std::array<std::optional<model::DomainError>, history_capacity>
      reconciliation_retention_evidence_{};
  std::array<std::optional<runtime::TurnKind>, history_capacity> callback_kinds_{};
  std::array<std::optional<runtime::AccountSafetyFenceTurn>, history_capacity> account_fences_{};
  std::array<std::optional<runtime::ControlTurnContext>, history_capacity>
      account_fence_contexts_{};
  std::size_t ordinary_view_count_{0U};
  std::size_t reconciliation_view_count_{0U};
  std::size_t ordinary_evidence_count_{0U};
  std::size_t reconciliation_evidence_count_{0U};
  std::size_t reconciliation_retention_evidence_count_{0U};
  std::size_t callback_count_{0U};
  std::size_t account_fence_count_{0U};
  mutable std::mutex evidence_mutex_;
  ReconciliationCompletionMode reconciliation_completion_mode_{
      ReconciliationCompletionMode::ConsumeApplied};
  risk::AccountSafetyReason reconciliation_retention_reason_{
      risk::AccountSafetyReason::TimeoutObserved};
  bool should_retain_reconciliation_token_{false};
  std::latch* reconciliation_completion_latch_{nullptr};
  std::optional<runtime::AdmittedReconciliationEventSlot> retained_reconciliation_token_;
  std::optional<runtime::GlobalPrivateFenceTurn> global_fence_;
  std::optional<runtime::ControlTurnContext> global_fence_context_;
};

// ########################################################################
// A fixed counting clock exposes whether a disabled or closed admission crossed the local receive
// observation boundary; each permitted observation returns the same monotonic value.
class CountingAdmissionClock final : public model::ClockProvider {
public:

  // --------------------------------------------------------
  // Retain the exact monotonic value returned by every permitted clock observation.
  explicit CountingAdmissionClock(std::uint64_t nanoseconds) noexcept : nanoseconds_{nanoseconds} {}

  // --------------------------------------------------------
  // Return the number of receive or processing observations made through the provider.
  [[nodiscard]] std::size_t observation_count() const noexcept { return observation_count_; }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Count and return one monotonic observation without introducing wall-clock nondeterminism.
  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override {
    ++observation_count_;
    return nanoseconds_;
  }

  // --------------------------------------------------------
  std::uint64_t nanoseconds_;
  std::size_t observation_count_{0U};
};

// ########################################################################
// One copied public probe retains the accepted context supplied by shared executor progression.
struct PublicTurnRecord {
  std::optional<runtime::AcceptedTurnContext> context;
};

// ########################################################################
// The inline public command borrows test storage whose lifetime encloses its queued execution.
struct PublicRecordCommand {
  PublicTurnRecord* record;
};

// ########################################################################

// --------------------------------------------------------
// Record one public accepted-turn context without creating another timing or ordering seam.
[[nodiscard]] model::Result<void>
record_public_turn(const PublicRecordCommand& command,
                   const runtime::AcceptedTurnContext& context) noexcept {
  command.record->context.emplace(context);
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Extract one sealed private-admission configuration or fail immediately for a fixture defect.
[[nodiscard]] runtime::PrivateAdmissionConfiguration
create_admission_configuration_or_throw(const test_support::M4TestAuthority& authority) {
  auto created = runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
      authority.configuration, authority.m4_policy);
  if (!created) {
    throw std::logic_error{"invalid reconciliation-admission test configuration"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Create a source factory sealed to the exact custom-capacity authority used by the executor.
[[nodiscard]] runtime::PrivateOrderEventFactory
create_event_factory_or_throw(const test_support::M4TestAuthority& authority) {
  auto resolver = runtime::M4ProvenanceResolver::create_m4_provenance_resolver(
      authority.configuration, authority.m4_policy);
  if (!resolver) {
    throw std::logic_error{"invalid reconciliation-admission provenance resolver"};
  }
  return runtime::PrivateOrderEventFactory{std::move(resolver).value()};
}

// --------------------------------------------------------
// Mint one configured ordinary-private timeout under the exact executor authority.
[[nodiscard]] oms::PrivateOrderIngressAttempt
create_private_timeout_attempt_or_throw(const test_support::M4PrivateEventFixture& fixture,
                                        const test_support::M4TestAuthority& authority,
                                        std::uint64_t event_counter) {
  const auto origin =
      fixture.create_local_private_event_origin_or_throw(event_counter, 100U + event_counter, 999U);
  auto created = create_event_factory_or_throw(authority).create_account_timeout_attempt(
      oms::LocalPrivateIngressOrigin{origin.event_id, origin.source_time}, fixture.account_id(),
      fixture.venue_id());
  if (!created) {
    throw std::logic_error{"invalid ordinary attempt in reconciliation-admission test"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Mint one configured authoritative acknowledgement under the exact executor authority.
[[nodiscard]] oms::ReconciliationPrivateEventIngressAttempt
create_reconciliation_attempt_or_throw(const test_support::M4PrivateEventFixture& fixture,
                                       const test_support::M4TestAuthority& authority,
                                       std::uint64_t row_ordinal,
                                       std::uint8_t exchange_identity_byte) {
  const auto origin = fixture.create_reconciliation_private_event_origin_or_throw(
      row_ordinal, 200U + row_ordinal, 999U);
  auto created =
      create_event_factory_or_throw(authority).create_reconciliation_acknowledgement_attempt(
          oms::ReconciliationPrivateIngressOrigin{origin.reconciliation_epoch_id,
                                                  origin.authoritative_cut_id, origin.row_ordinal,
                                                  origin.cut_time},
          fixture.account_id(), fixture.venue_id(),
          test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(
              exchange_identity_byte),
          fixture.outbound_order_record().order_id(), fixture.instrument_id());
  if (!created) {
    throw std::logic_error{"invalid authoritative attempt in reconciliation-admission test"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Mint one authoritative row under the fixture's foreign default root while retaining account and
// venue text that match the custom-capacity executor.
[[nodiscard]] oms::ReconciliationPrivateEventIngressAttempt
create_foreign_reconciliation_attempt_or_throw(const test_support::M4PrivateEventFixture& fixture,
                                               std::uint64_t row_ordinal,
                                               std::uint8_t exchange_identity_byte) {
  const auto origin = fixture.create_reconciliation_private_event_origin_or_throw(
      row_ordinal, 300U + row_ordinal, 999U);
  auto created = fixture.private_event_factory().create_reconciliation_acknowledgement_attempt(
      oms::ReconciliationPrivateIngressOrigin{origin.reconciliation_epoch_id,
                                              origin.authoritative_cut_id, origin.row_ordinal,
                                              origin.cut_time},
      fixture.account_id(), fixture.venue_id(),
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(
          exchange_identity_byte),
      fixture.outbound_order_record().order_id(), fixture.instrument_id());
  if (!created) {
    throw std::logic_error{
        "invalid foreign-root reconciliation attempt in reconciliation-admission test"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Mint one foreign-root ordinary attempt whose account and venue text still match the executor.
[[nodiscard]] oms::PrivateOrderIngressAttempt
create_foreign_private_attempt_or_throw(const test_support::M4PrivateEventFixture& fixture,
                                        std::uint64_t event_counter) {
  const auto origin =
      fixture.create_local_private_event_origin_or_throw(event_counter, 100U + event_counter, 999U);
  auto created = fixture.private_event_factory().create_account_timeout_attempt(
      oms::LocalPrivateIngressOrigin{origin.event_id, origin.source_time}, fixture.account_id(),
      fixture.venue_id());
  if (!created) {
    throw std::logic_error{"invalid foreign-root attempt in reconciliation-admission test"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Disabled reconciliation ingress creates no counter or clock observation, while explicit closure
// consumes only its shared attempt ordinal and leaves accepted-only receive authority untouched.
TEST_CASE("reconciliation admission preserves disabled and closed boundaries",
          "[runtime][m4][reconciliation-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_reconciliation_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  const auto attempt = create_reconciliation_attempt_or_throw(fixture, authority, 1U, 0x60U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A legacy executor rejects before assigning an ordinal; its first later public admission proves
  // the shared attempt and receive counters both remained at their pre-first values.
  CountingAdmissionClock disabled_clock{5U};
  runtime::SerializedExecutor disabled_executor{1U, disabled_clock};
  const auto disabled = disabled_executor.try_admit_reconciliation_event(attempt);
  REQUIRE_FALSE(disabled);
  CHECK(disabled.error() ==
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "reconciliation_admission.disabled"));
  CHECK(disabled_clock.observation_count() == 0U);
  PublicTurnRecord public_record;
  const auto first_public = disabled_executor.try_admit(
      runtime::InlineCommandWorkItem::create_inline_command_work_item<&record_public_turn>(
          PublicRecordCommand{&public_record}));
  REQUIRE(first_public);
  REQUIRE(first_public.value().receipt.has_value());
  CHECK(first_public.value().attempt_ordinal.value() == 1U);
  CHECK(first_public.value().receipt->receive_sequence.value() == 1U);
  CHECK(disabled_clock.observation_count() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Configured closure records an explicit attempt decision but does not inspect the clock or mint
  // a receipt whose receive sequence would falsely claim that the row entered the reserve.
  ReconciliationAdmissionOwner owner;
  CountingAdmissionClock closed_clock{7U};
  runtime::SerializedExecutor closed_executor{
      0U, closed_clock, create_admission_configuration_or_throw(authority), owner};
  closed_executor.close();
  const auto closed = closed_executor.try_admit_reconciliation_event(attempt);
  REQUIRE(closed);
  CHECK(closed.value().outcome == runtime::AdmissionOutcome::Closed);
  CHECK(closed.value().attempt_ordinal.value() == 1U);
  CHECK(closed.value().pending_depth == 0U);
  CHECK(closed.value().pending_capacity == 1U);
  CHECK_FALSE(closed.value().receipt.has_value());
  CHECK_FALSE(closed.value().state.has_value());
  CHECK_FALSE(closed.value().account_fence_recorded);
  CHECK(closed_clock.observation_count() == 0U);
  CHECK_FALSE(closed_executor.reconciliation_admission_observation(closed.value().attempt_ordinal)
                  .has_value());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Ordinary-private, reconciliation, and public queues keep separate capacity while their accepted
// values share one exact attempt, receive, turn, clock, and evidence ordering domain.
TEST_CASE("reconciliation admission isolates capacity and shares executor order",
          "[runtime][m4][reconciliation-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_private_admissions = 1U;
  capacities.max_reconciliation_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  ReconciliationAdmissionOwner owner;
  model::DeterministicClockProvider clock{10U};
  runtime::SerializedExecutor executor{1U, clock,
                                       create_admission_configuration_or_throw(authority), owner};
  runtime::DeterministicExecutorDriver driver{executor};
  PublicTurnRecord public_record;
  const auto private_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 1U);
  const auto reconciliation_attempt =
      create_reconciliation_attempt_or_throw(fixture, authority, 1U, 0x61U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Fill all three one-slot reserves; acceptance in each proves no lane borrowed another's slot.
  const auto private_admission = executor.try_admit_private(private_attempt);
  REQUIRE(private_admission);
  REQUIRE(private_admission.value().receipt.has_value());
  REQUIRE(clock.advance_nanoseconds(1U));
  const auto reconciliation_admission =
      executor.try_admit_reconciliation_event(reconciliation_attempt);
  REQUIRE(reconciliation_admission);
  REQUIRE(reconciliation_admission.value().receipt.has_value());
  REQUIRE(clock.advance_nanoseconds(1U));
  const auto public_admission = executor.try_admit(
      runtime::InlineCommandWorkItem::create_inline_command_work_item<&record_public_turn>(
          PublicRecordCommand{&public_record}));
  REQUIRE(public_admission);
  REQUIRE(public_admission.value().receipt.has_value());

  CHECK(private_admission.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(private_admission.value().attempt_ordinal.value() == 1U);
  CHECK(private_admission.value().receipt->receive_sequence.value() == 1U);
  CHECK(private_admission.value().receipt->received_at == model::ReceiveTimestamp{10U});
  CHECK(reconciliation_admission.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(reconciliation_admission.value().attempt_ordinal.value() == 2U);
  CHECK(reconciliation_admission.value().pending_depth == 1U);
  CHECK(reconciliation_admission.value().pending_capacity == 1U);
  CHECK(reconciliation_admission.value().receipt->receive_sequence.value() == 2U);
  CHECK(reconciliation_admission.value().receipt->received_at == model::ReceiveTimestamp{11U});
  CHECK(public_admission.value().attempt_ordinal.value() == 3U);
  CHECK(public_admission.value().receipt->receive_sequence.value() == 3U);
  CHECK(public_admission.value().receipt->received_at == model::ReceiveTimestamp{12U});
  const auto full_snapshot = executor.private_lane_snapshot();
  CHECK(full_snapshot.queued_slots == 1U);
  CHECK(full_snapshot.private_capacity == 1U);
  CHECK(full_snapshot.reconciliation_queued_slots == 1U);
  CHECK(full_snapshot.reconciliation_capacity == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The globally oldest accepted attempt executes first across all three physical reserves.
  REQUIRE(driver.bind_to_current_thread());
  REQUIRE(clock.advance_nanoseconds(8U));
  const auto private_turn = driver.execute_next_turn();
  REQUIRE(private_turn);
  REQUIRE(private_turn.value().has_value());
  CHECK(private_turn.value()->kind == runtime::TurnKind::PrivateCommand);
  CHECK(private_turn.value()->attempt_ordinal.value() == 1U);
  CHECK(private_turn.value()->turn_ordinal.value() == 1U);
  REQUIRE(clock.advance_nanoseconds(1U));
  const auto reconciliation_turn = driver.execute_next_turn();
  REQUIRE(reconciliation_turn);
  REQUIRE(reconciliation_turn.value().has_value());
  CHECK(reconciliation_turn.value()->kind == runtime::TurnKind::ReconciliationCommand);
  CHECK(reconciliation_turn.value()->attempt_ordinal.value() == 2U);
  CHECK(reconciliation_turn.value()->turn_ordinal.value() == 2U);
  REQUIRE(clock.advance_nanoseconds(1U));
  const auto public_turn = driver.execute_next_turn();
  REQUIRE(public_turn);
  REQUIRE(public_turn.value().has_value());
  CHECK(public_turn.value()->kind == runtime::TurnKind::Command);
  CHECK(public_turn.value()->attempt_ordinal.value() == 3U);
  CHECK(public_turn.value()->turn_ordinal.value() == 3U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Each lane resolves terminal evidence only through its matching owner oracle namespace.
  REQUIRE(owner.ordinary_view_count() == 1U);
  CHECK(owner.ordinary_view_at(0U).ingress_attempt() == private_attempt);
  REQUIRE(owner.reconciliation_view_count() == 1U);
  CHECK(owner.reconciliation_view_at(0U).ingress_attempt() == reconciliation_attempt);
  CHECK(owner.reconciliation_view_at(0U).admission_receipt() ==
        reconciliation_admission.value().receipt.value());
  CHECK(owner.reconciliation_view_at(0U).queue_age() == model::ElapsedNanoseconds{10U});
  REQUIRE(public_record.context.has_value());
  CHECK(public_record.context->receipt == public_admission.value().receipt.value());
  const auto private_terminal =
      executor.private_admission_observation(private_admission.value().attempt_ordinal);
  const auto reconciliation_terminal = executor.reconciliation_admission_observation(
      reconciliation_admission.value().attempt_ordinal);
  REQUIRE(private_terminal.has_value());
  REQUIRE(reconciliation_terminal.has_value());
  CHECK(private_terminal->state == runtime::CriticalPrivateAdmissionState::EconomicallyConsumed);
  CHECK(reconciliation_terminal->state ==
        runtime::CriticalPrivateAdmissionState::EconomicallyConsumed);
  CHECK(executor.reconciliation_admission_observation(private_admission.value().attempt_ordinal) ==
        std::nullopt);
  CHECK(executor.private_admission_observation(reconciliation_admission.value().attempt_ordinal) ==
        std::nullopt);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Simultaneous reconciliation producers linearize into one contiguous global attempt sequence,
// copy exactly the bounded prefix, and retain every rejected fact in caller-owned storage.
TEST_CASE("concurrent reconciliation producers preserve bounded explicit admission",
          "[runtime][m4][reconciliation-admission][concurrency]") {
  constexpr std::size_t producer_count = 8U;
  constexpr std::size_t reconciliation_capacity = 3U;
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_reconciliation_admissions = reconciliation_capacity;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  ReconciliationAdmissionOwner owner;
  model::SystemClockProvider clock;
  runtime::SerializedExecutor executor{0U, clock,
                                       create_admission_configuration_or_throw(authority), owner};
  std::array<std::optional<oms::ReconciliationPrivateEventIngressAttempt>, producer_count>
      attempts{};
  std::array<std::optional<oms::ReconciliationPrivateEventIngressAttempt>, producer_count>
      original_attempts{};
  std::array<std::optional<runtime::ReconciliationAdmissionDecision>, producer_count> decisions{};
  std::array<std::thread, producer_count> producers{};
  std::latch ready{producer_count};
  std::latch start{1U};

  // ++++++++++++++++++++++++++++++++++++++++
  // Build distinguishable caller-owned rows before releasing all producer threads together.
  for (std::size_t index = 0U; index < producer_count; ++index) {
    attempts[index].emplace(create_reconciliation_attempt_or_throw(
        fixture, authority, 10U + index, static_cast<std::uint8_t>(0x70U + index)));
    original_attempts[index].emplace(attempts[index].value());
    producers[index] = std::thread{[&executor, &attempts, &decisions, &ready, &start, index] {
      ready.count_down();
      start.wait();
      const auto decision = executor.try_admit_reconciliation_event(attempts[index].value());
      if (decision) {
        decisions[index].emplace(decision.value());
      }
    }};
  }
  ready.wait();
  start.count_down();
  for (auto& producer : producers) {
    producer.join();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Arbitrary mutex acquisition chooses input order, but it cannot duplicate or skip an ordinal;
  // only the exact first three linearized attempts receive copied slots and receive sequences.
  std::array<bool, producer_count + 1U> seen_ordinals{};
  std::array<std::size_t, producer_count + 1U> input_index_by_ordinal{};
  for (std::size_t index = 0U; index < producer_count; ++index) {
    REQUIRE(decisions[index].has_value());
    const auto ordinal = static_cast<std::size_t>(decisions[index]->attempt_ordinal.value());
    REQUIRE(ordinal > 0U);
    REQUIRE(ordinal <= producer_count);
    CHECK_FALSE(seen_ordinals[ordinal]);
    seen_ordinals[ordinal] = true;
    input_index_by_ordinal[ordinal] = index;
    CHECK(decisions[index]->pending_capacity == reconciliation_capacity);
    CHECK(attempts[index] == original_attempts[index]);
    if (ordinal <= reconciliation_capacity) {
      CHECK(decisions[index]->outcome == runtime::AdmissionOutcome::Accepted);
      CHECK(decisions[index]->pending_depth == ordinal);
      REQUIRE(decisions[index]->receipt.has_value());
      CHECK(decisions[index]->receipt->receive_sequence.value() == ordinal);
      CHECK_FALSE(decisions[index]->account_fence_recorded);
    } else {
      CHECK(decisions[index]->outcome == runtime::AdmissionOutcome::CapacityExceeded);
      CHECK(decisions[index]->pending_depth == reconciliation_capacity);
      CHECK_FALSE(decisions[index]->receipt.has_value());
      CHECK(decisions[index]->account_fence_recorded);
    }
  }
  for (std::size_t ordinal = 1U; ordinal <= producer_count; ++ordinal) {
    CHECK(seen_ordinals[ordinal]);
  }
  const auto full_snapshot = executor.private_lane_snapshot();
  CHECK(full_snapshot.queued_slots == 0U);
  CHECK(full_snapshot.reconciliation_queued_slots == reconciliation_capacity);
  CHECK(full_snapshot.reconciliation_capacity == reconciliation_capacity);
  CHECK(full_snapshot.pending_account_fences == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The deterministic owner drains the three accepted copies in ordinal order and then applies one
  // fence containing every explicit rejected attempt without inventing a receive observation.
  runtime::DeterministicExecutorDriver driver{executor};
  REQUIRE(driver.bind_to_current_thread());
  const auto drained = driver.execute_pending_turns(producer_count);
  REQUIRE(drained);
  CHECK(drained.value().turns_executed == reconciliation_capacity + 1U);
  CHECK(owner.reconciliation_view_count() == reconciliation_capacity);
  for (std::size_t ordinal = 1U; ordinal <= reconciliation_capacity; ++ordinal) {
    const auto input_index = input_index_by_ordinal[ordinal];
    CHECK(owner.reconciliation_view_at(ordinal - 1U).ingress_attempt() ==
          attempts[input_index].value());
  }
  REQUIRE(owner.account_fence_count() == 1U);
  CHECK(owner.account_fence_at(0U).lost_attempt_count == producer_count - reconciliation_capacity);
  REQUIRE(owner.account_fence_at(0U).reason_occurrence_count == 1U);
  const auto& first_loss = owner.account_fence_at(0U).ordered_unique_reason_occurrences[0U].value();
  CHECK(first_loss.first_attempt_ordinal.value() == reconciliation_capacity + 1U);
  const auto* first_lost_reconciliation =
      std::get_if<oms::ReconciliationPrivateEventIngressAttempt>(&first_loss.first_attempt);
  REQUIRE(first_lost_reconciliation != nullptr);
  CHECK(*first_lost_reconciliation ==
        attempts[input_index_by_ordinal[reconciliation_capacity + 1U]].value());
  REQUIRE(driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A trusted configured reconciliation row bypasses a permanent global-private gate, while the
// executor still delivers any older pending global fence before that row.
TEST_CASE("reconciliation admission follows older global fences but bypasses their permanent gate",
          "[runtime][m4][reconciliation-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_private_admissions = 1U;
  capacities.max_reconciliation_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  ReconciliationAdmissionOwner owner;
  model::DeterministicClockProvider clock{30U};
  runtime::SerializedExecutor executor{0U, clock,
                                       create_admission_configuration_or_throw(authority), owner};
  runtime::DeterministicExecutorDriver driver{executor};
  const auto foreign_attempt = create_foreign_private_attempt_or_throw(fixture, 31U);
  const auto queued_reconciliation =
      create_reconciliation_attempt_or_throw(fixture, authority, 2U, 0x62U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A foreign sealed root creates ordinal one's reasonless fence; configured reconciliation still
  // enters its own reserve at ordinal two before that fence is owner-applied.
  const auto global_loss = executor.try_admit_private(foreign_attempt);
  REQUIRE(global_loss);
  CHECK(global_loss.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(global_loss.value().attempt_ordinal.value() == 1U);
  CHECK_FALSE(global_loss.value().account_fence_recorded);
  const auto accepted = executor.try_admit_reconciliation_event(queued_reconciliation);
  REQUIRE(accepted);
  CHECK(accepted.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(accepted.value().attempt_ordinal.value() == 2U);
  CHECK(executor.private_lane_snapshot().global_fence_active);
  CHECK_FALSE(executor.private_lane_snapshot().global_fence_owner_applied);

  // ++++++++++++++++++++++++++++++++++++++++
  // Shared ordinal selection applies the older fence before executing the queued trusted row.
  REQUIRE(driver.bind_to_current_thread());
  const auto global_turn = driver.execute_next_turn();
  REQUIRE(global_turn);
  REQUIRE(global_turn.value().has_value());
  CHECK(global_turn.value()->kind == runtime::TurnKind::GlobalPrivateFence);
  CHECK(global_turn.value()->attempt_ordinal.value() == 1U);
  REQUIRE(owner.global_fence().has_value());
  REQUIRE(
      std::holds_alternative<oms::PrivateOrderIngressAttempt>(owner.global_fence()->first_attempt));
  CHECK(std::get<oms::PrivateOrderIngressAttempt>(owner.global_fence()->first_attempt) ==
        foreign_attempt);
  REQUIRE(owner.global_fence_context().has_value());
  CHECK(owner.global_fence_context()->turn_ordinal.value() == 1U);
  CHECK(executor.private_lane_snapshot().global_fence_owner_applied);

  const auto queued_turn = driver.execute_next_turn();
  REQUIRE(queued_turn);
  REQUIRE(queued_turn.value().has_value());
  CHECK(queued_turn.value()->kind == runtime::TurnKind::ReconciliationCommand);
  CHECK(queued_turn.value()->attempt_ordinal.value() == 2U);
  CHECK(queued_turn.value()->turn_ordinal.value() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A later configured row is accepted and executed even after the global gate becomes permanent.
  const auto later_reconciliation =
      create_reconciliation_attempt_or_throw(fixture, authority, 3U, 0x63U);
  const auto later_admission = executor.try_admit_reconciliation_event(later_reconciliation);
  REQUIRE(later_admission);
  CHECK(later_admission.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(later_admission.value().attempt_ordinal.value() == 3U);
  CHECK(later_admission.value().receipt->receive_sequence.value() == 2U);
  const auto later_turn = driver.execute_next_turn();
  REQUIRE(later_turn);
  REQUIRE(later_turn.value().has_value());
  CHECK(later_turn.value()->kind == runtime::TurnKind::ReconciliationCommand);
  CHECK(later_turn.value()->attempt_ordinal.value() == 3U);
  CHECK(later_turn.value()->turn_ordinal.value() == 3U);
  CHECK(owner.callback_count() == 3U);
  CHECK(owner.callback_kind_at(0U) == runtime::TurnKind::GlobalPrivateFence);
  CHECK(owner.callback_kind_at(1U) == runtime::TurnKind::ReconciliationCommand);
  CHECK(owner.callback_kind_at(2U) == runtime::TurnKind::ReconciliationCommand);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A reconciliation row sealed under a foreign M4 root is unattributable even when its account and
// venue text match; the reasonless global fence retains its nominal reconciliation alternative.
TEST_CASE("foreign-root reconciliation admission records its exact global-fence variant",
          "[runtime][m4][reconciliation-admission][global-fence]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_reconciliation_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  ReconciliationAdmissionOwner owner;
  model::DeterministicClockProvider clock{45U};
  runtime::SerializedExecutor executor{0U, clock,
                                       create_admission_configuration_or_throw(authority), owner};
  runtime::DeterministicExecutorDriver driver{executor};
  const auto foreign_attempt = create_foreign_reconciliation_attempt_or_throw(fixture, 30U, 0x7eU);

  // ++++++++++++++++++++++++++++++++++++++++
  // Exact-root attribution fails before receive-time observation or reconciliation-ring insertion.
  const auto rejected = executor.try_admit_reconciliation_event(foreign_attempt);
  REQUIRE(rejected);
  CHECK(rejected.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(rejected.value().attempt_ordinal.value() == 1U);
  CHECK(rejected.value().pending_depth == 0U);
  CHECK(rejected.value().pending_capacity == 1U);
  CHECK_FALSE(rejected.value().receipt.has_value());
  CHECK_FALSE(rejected.value().account_fence_recorded);
  CHECK_FALSE(
      executor.reconciliation_admission_observation(rejected.value().attempt_ordinal).has_value());
  CHECK(executor.private_lane_snapshot().reconciliation_queued_slots == 0U);
  CHECK(executor.private_lane_snapshot().global_fence_active);

  // ++++++++++++++++++++++++++++++++++++++++
  // The sole control turn preserves the complete authoritative value under its reconciliation tag.
  REQUIRE(driver.bind_to_current_thread());
  const auto global_turn = driver.execute_next_turn();
  REQUIRE(global_turn);
  REQUIRE(global_turn.value().has_value());
  CHECK(global_turn.value()->kind == runtime::TurnKind::GlobalPrivateFence);
  CHECK(global_turn.value()->attempt_ordinal == rejected.value().attempt_ordinal);
  REQUIRE(owner.global_fence().has_value());
  CHECK(owner.global_fence()->earliest_attempt_ordinal == rejected.value().attempt_ordinal);
  CHECK(owner.global_fence()->lost_attempt_count == 1U);
  const auto* retained_reconciliation_attempt =
      std::get_if<oms::ReconciliationPrivateEventIngressAttempt>(
          &owner.global_fence()->first_attempt);
  REQUIRE(retained_reconciliation_attempt != nullptr);
  CHECK(*retained_reconciliation_attempt == foreign_attempt);
  CHECK(executor.private_lane_snapshot().global_fence_owner_applied);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Reconciliation capacity loss and later same-account attempts fold into one configured fence that
// retains the first nominal authoritative fact and releases its gate only after owner application.
TEST_CASE("reconciliation admission folds configured account losses",
          "[runtime][m4][reconciliation-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_reconciliation_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  ReconciliationAdmissionOwner owner;
  model::DeterministicClockProvider clock{50U};
  runtime::SerializedExecutor executor{0U, clock,
                                       create_admission_configuration_or_throw(authority), owner};
  runtime::DeterministicExecutorDriver driver{executor};
  const auto accepted_attempt =
      create_reconciliation_attempt_or_throw(fixture, authority, 4U, 0x64U);
  const auto first_lost_attempt =
      create_reconciliation_attempt_or_throw(fixture, authority, 5U, 0x65U);
  const auto folded_attempt = create_reconciliation_attempt_or_throw(fixture, authority, 6U, 0x66U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The full reconciliation reserve records its first loss, and the resulting account gate folds a
  // later attempt without changing the queued depth or consuming a receive sequence.
  const auto accepted = executor.try_admit_reconciliation_event(accepted_attempt);
  const auto first_loss = executor.try_admit_reconciliation_event(first_lost_attempt);
  const auto folded_loss = executor.try_admit_reconciliation_event(folded_attempt);
  REQUIRE(accepted);
  REQUIRE(first_loss);
  REQUIRE(folded_loss);
  CHECK(accepted.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(accepted.value().attempt_ordinal.value() == 1U);
  CHECK(first_loss.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(first_loss.value().attempt_ordinal.value() == 2U);
  CHECK(first_loss.value().pending_depth == 1U);
  CHECK(first_loss.value().pending_capacity == 1U);
  CHECK(first_loss.value().account_fence_recorded);
  CHECK(folded_loss.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(folded_loss.value().attempt_ordinal.value() == 3U);
  CHECK(folded_loss.value().account_fence_recorded);
  const auto snapshot =
      executor.find_account_safety_fence_snapshot(fixture.account_id(), fixture.venue_id());
  REQUIRE(snapshot.has_value());
  CHECK(snapshot->earliest_pending_attempt_ordinal == first_loss.value().attempt_ordinal);
  CHECK(snapshot->pending_lost_attempt_count == 2U);
  REQUIRE(snapshot->reason_occurrence_count == 1U);
  REQUIRE(snapshot->ordered_unique_reason_occurrences[0U].has_value());
  CHECK(snapshot->ordered_unique_reason_occurrences[0U]->reason ==
        risk::AccountSafetyReason::CriticalAdmissionLoss);
  CHECK(snapshot->ordered_unique_reason_occurrences[0U]->first_attempt_ordinal ==
        first_loss.value().attempt_ordinal);

  // ++++++++++++++++++++++++++++++++++++++++
  // Consumption frees the physical slot, but the older account fence still executes before later
  // same-account reconciliation can be accepted.
  REQUIRE(driver.bind_to_current_thread());
  const auto accepted_turn = driver.execute_next_turn();
  REQUIRE(accepted_turn);
  REQUIRE(accepted_turn.value().has_value());
  CHECK(accepted_turn.value()->kind == runtime::TurnKind::ReconciliationCommand);
  CHECK(accepted_turn.value()->attempt_ordinal.value() == 1U);
  const auto fence_turn = driver.execute_next_turn();
  REQUIRE(fence_turn);
  REQUIRE(fence_turn.value().has_value());
  CHECK(fence_turn.value()->kind == runtime::TurnKind::AccountSafetyFence);
  CHECK(fence_turn.value()->attempt_ordinal.value() == 2U);
  CHECK(fence_turn.value()->turn_ordinal.value() == 2U);
  REQUIRE(owner.account_fence_count() == 1U);
  const auto& delivered_fence = owner.account_fence_at(0U);
  CHECK(delivered_fence.logical_account_id == fixture.account_id());
  CHECK(delivered_fence.venue_id == fixture.venue_id());
  CHECK(delivered_fence.lost_attempt_count == 2U);
  REQUIRE(delivered_fence.reason_occurrence_count == 1U);
  REQUIRE(delivered_fence.ordered_unique_reason_occurrences[0U].has_value());
  const auto& occurrence = delivered_fence.ordered_unique_reason_occurrences[0U].value();
  CHECK(occurrence.first_attempt_ordinal == first_loss.value().attempt_ordinal);
  REQUIRE(std::holds_alternative<oms::ReconciliationPrivateEventIngressAttempt>(
      occurrence.first_attempt));
  CHECK(std::get<oms::ReconciliationPrivateEventIngressAttempt>(occurrence.first_attempt) ==
        first_lost_attempt);
  CHECK(owner.account_fence_context_at(0U).turn_ordinal.value() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Successful fence application clears the account gate; rejected attempts did not advance the
  // accepted-only receive sequence used by the next row.
  const auto successor_attempt =
      create_reconciliation_attempt_or_throw(fixture, authority, 7U, 0x67U);
  const auto successor = executor.try_admit_reconciliation_event(successor_attempt);
  REQUIRE(successor);
  CHECK(successor.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(successor.value().attempt_ordinal.value() == 4U);
  REQUIRE(successor.value().receipt.has_value());
  CHECK(successor.value().receipt->receive_sequence.value() == 2U);
  CHECK(executor.private_lane_snapshot().pending_account_fences == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A valid retained reconciliation completion remains observable only through its lane-specific
// error oracle and records the complete authoritative attempt in one configured-account fence.
TEST_CASE("retained reconciliation completion publishes separate evidence and account containment",
          "[runtime][m4][reconciliation-admission][retention]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_reconciliation_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  ReconciliationAdmissionOwner owner;
  owner.select_reconciliation_completion(
      ReconciliationAdmissionOwner::ReconciliationCompletionMode::
          RetainAccountWithCommittedEvidence,
      risk::AccountSafetyReason::TimeoutObserved);
  model::DeterministicClockProvider clock{60U};
  runtime::SerializedExecutor executor{0U, clock,
                                       create_admission_configuration_or_throw(authority), owner};
  runtime::DeterministicExecutorDriver driver{executor};
  const auto attempt = create_reconciliation_attempt_or_throw(fixture, authority, 8U, 0x68U);
  const auto admitted = executor.try_admit_reconciliation_event(attempt);
  REQUIRE(admitted);
  REQUIRE(admitted.value().receipt.has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Owner consumption succeeds only after its matching reconciliation retention error is published.
  REQUIRE(driver.bind_to_current_thread());
  const auto retained_turn = driver.execute_next_turn();
  REQUIRE(retained_turn);
  REQUIRE(retained_turn.value().has_value());
  CHECK(retained_turn.value()->kind == runtime::TurnKind::ReconciliationCommand);
  const auto expected_error = model::DomainError::create_at_field(
      model::DomainErrorCode::InvalidPrivateEvent, "test_reconciliation_retention");
  const auto* committed_reconciliation_error =
      owner.find_committed_retained_reconciliation_event_error(admitted.value().attempt_ordinal);
  REQUIRE(committed_reconciliation_error != nullptr);
  CHECK(*committed_reconciliation_error == expected_error);
  CHECK(owner.find_committed_retained_private_event_error(admitted.value().attempt_ordinal) ==
        nullptr);
  const auto retained =
      executor.reconciliation_admission_observation(admitted.value().attempt_ordinal);
  REQUIRE(retained.has_value());
  CHECK(retained->state == runtime::CriticalPrivateAdmissionState::RetainedForReconciliation);
  CHECK_FALSE(retained->disposition.has_value());
  CHECK(retained->retention_error == expected_error);
  CHECK(executor.private_admission_observation(admitted.value().attempt_ordinal) == std::nullopt);

  // ++++++++++++++++++++++++++++++++++++++++
  // The resulting account fence carries the owner-selected reason and the exact nominal
  // reconciliation attempt rather than converting it to ordinary-private authority.
  const auto pending =
      executor.find_account_safety_fence_snapshot(fixture.account_id(), fixture.venue_id());
  REQUIRE(pending.has_value());
  CHECK(pending->pending_lost_attempt_count == 1U);
  CHECK(pending->earliest_pending_attempt_ordinal == admitted.value().attempt_ordinal);
  REQUIRE(pending->reason_occurrence_count == 1U);
  REQUIRE(pending->ordered_unique_reason_occurrences[0U].has_value());
  CHECK(pending->ordered_unique_reason_occurrences[0U]->reason ==
        risk::AccountSafetyReason::TimeoutObserved);
  const auto fence_turn = driver.execute_next_turn();
  REQUIRE(fence_turn);
  REQUIRE(fence_turn.value().has_value());
  CHECK(fence_turn.value()->kind == runtime::TurnKind::AccountSafetyFence);
  REQUIRE(owner.account_fence_count() == 1U);
  const auto& occurrence = owner.account_fence_at(0U).ordered_unique_reason_occurrences[0U].value();
  CHECK(occurrence.reason == risk::AccountSafetyReason::TimeoutObserved);
  CHECK(occurrence.first_attempt_ordinal == admitted.value().attempt_ordinal);
  const auto* retained_reconciliation_attempt =
      std::get_if<oms::ReconciliationPrivateEventIngressAttempt>(&occurrence.first_attempt);
  REQUIRE(retained_reconciliation_attempt != nullptr);
  CHECK(*retained_reconciliation_attempt == attempt);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A reconciliation owner that contradicts its committed disposition or omits retained evidence is
// terminally rejected, while exact account containment remains pending for the copied source fact.
TEST_CASE("invalid reconciliation completion evidence fails closed with account containment",
          "[runtime][m4][reconciliation-admission][retention]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_reconciliation_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;

  // ++++++++++++++++++++++++++++++++++++++++
  // Exercise both independent oracle violations through the complete owner-turn failure boundary.
  const auto prove_invalid_reconciliation_completion_fails_closed =
      [&](ReconciliationAdmissionOwner::ReconciliationCompletionMode mode,
          const model::DomainError& expected_error, std::uint64_t row_ordinal,
          std::uint8_t exchange_identity_byte) {
        ReconciliationAdmissionOwner owner;
        owner.select_reconciliation_completion(mode);
        model::DeterministicClockProvider clock{70U};
        runtime::SerializedExecutor executor{
            0U, clock, create_admission_configuration_or_throw(authority), owner};
        runtime::DeterministicExecutorDriver driver{executor};
        const auto attempt = create_reconciliation_attempt_or_throw(fixture, authority, row_ordinal,
                                                                    exchange_identity_byte);
        const auto admitted = executor.try_admit_reconciliation_event(attempt);
        REQUIRE(admitted);
        REQUIRE(driver.bind_to_current_thread());
        const auto failed_turn = driver.execute_next_turn();
        REQUIRE_FALSE(failed_turn);
        CHECK(failed_turn.error() == expected_error);
        CHECK(executor.terminal_error() == expected_error);

        // ++++++++++++++++++++++++++++++++++++++++
        // The lane-specific invalid observation retains the exact failure class and cannot appear
        // through the ordinary-private observation namespace.
        const auto invalid =
            executor.reconciliation_admission_observation(admitted.value().attempt_ordinal);
        REQUIRE(invalid.has_value());
        CHECK(invalid->state == runtime::CriticalPrivateAdmissionState::RetainedForReconciliation);
        CHECK_FALSE(invalid->disposition.has_value());
        CHECK(invalid->retention_error == expected_error);
        CHECK(executor.private_admission_observation(admitted.value().attempt_ordinal) ==
              std::nullopt);
        REQUIRE(owner.reconciliation_view_count() == 1U);
        CHECK(owner.reconciliation_view_at(0U).ingress_attempt() == attempt);
        if (mode == ReconciliationAdmissionOwner::ReconciliationCompletionMode::
                        ConsumeWithMismatchedDisposition) {
          CHECK(owner.find_committed_reconciliation_event_disposition(
                    admitted.value().attempt_ordinal) ==
                oms::PrivateEventDisposition::ProjectionOnly);
        } else {
          CHECK(owner.find_committed_reconciliation_event_disposition(
                    admitted.value().attempt_ordinal) == std::nullopt);
        }
        CHECK(owner.find_committed_retained_reconciliation_event_error(
                  admitted.value().attempt_ordinal) == nullptr);

        // ++++++++++++++++++++++++++++++++++++++++
        // Fail-closed progression cannot deliver the full fence, so its public bounded snapshot is
        // the lawful proof that the copied reconciliation source remains conservatively contained.
        const auto containment =
            executor.find_account_safety_fence_snapshot(fixture.account_id(), fixture.venue_id());
        REQUIRE(containment.has_value());
        CHECK(containment->earliest_pending_attempt_ordinal == admitted.value().attempt_ordinal);
        CHECK(containment->pending_lost_attempt_count == 1U);
        REQUIRE(containment->reason_occurrence_count == 1U);
        REQUIRE(containment->ordered_unique_reason_occurrences[0U].has_value());
        CHECK(containment->ordered_unique_reason_occurrences[0U]->reason ==
              risk::AccountSafetyReason::CriticalAdmissionLoss);
        CHECK(containment->ordered_unique_reason_occurrences[0U]->first_attempt_ordinal ==
              admitted.value().attempt_ordinal);
        CHECK(owner.account_fence_count() == 0U);

        // ++++++++++++++++++++++++++++++++++++++++
      };

  // ++++++++++++++++++++++++++++++++++++++++
  // A consumed Applied claim cannot be satisfied by a committed ProjectionOnly disposition.
  prove_invalid_reconciliation_completion_fails_closed(
      ReconciliationAdmissionOwner::ReconciliationCompletionMode::ConsumeWithMismatchedDisposition,
      model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidPrivateEvent,
          "reconciliation_admission.find_committed_reconciliation_event_disposition"),
      9U, 0x69U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A retained completion without its matching stable oracle error cannot become terminal evidence.
  prove_invalid_reconciliation_completion_fails_closed(
      ReconciliationAdmissionOwner::ReconciliationCompletionMode::
          RetainAccountWithoutCommittedEvidence,
      model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                          "reconciliation_admission.retained_completion"),
      10U, 0x6aU);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The dedicated owner-thread driver reaches the same ReconciliationCommand processor and publishes
// the same terminal evidence and turn report as deterministic manual progression.
TEST_CASE("dedicated driver executes reconciliation commands through the shared processor",
          "[runtime][m4][reconciliation-admission][dedicated-driver]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_reconciliation_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  ReconciliationAdmissionOwner owner;
  std::latch completion{1U};
  owner.signal_reconciliation_completion(completion);
  model::SystemClockProvider clock;
  runtime::SerializedExecutor executor{0U, clock,
                                       create_admission_configuration_or_throw(authority), owner};
  const auto attempt = create_reconciliation_attempt_or_throw(fixture, authority, 20U, 0x7fU);
  const auto admitted = executor.try_admit_reconciliation_event(attempt);
  REQUIRE(admitted);
  REQUIRE(admitted.value().receipt.has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Startup binds the production owner thread, which wakes for the already queued trusted row.
  runtime::DedicatedExecutorDriver driver{executor};
  REQUIRE(driver.has_started_successfully());
  completion.wait();
  executor.close();
  driver.wait_until_stopped();

  // ++++++++++++++++++++++++++++++++++++++++
  // Closed-empty shutdown releases ownership after publishing the reconciliation turn report.
  CHECK_FALSE(driver.is_running());
  CHECK_FALSE(driver.terminal_error().has_value());
  REQUIRE(driver.last_turn_report().has_value());
  CHECK(driver.last_turn_report()->kind == runtime::TurnKind::ReconciliationCommand);
  CHECK(driver.last_turn_report()->attempt_ordinal == admitted.value().attempt_ordinal);
  CHECK(driver.last_turn_report()->turn_ordinal.value() == 1U);
  CHECK(owner.reconciliation_view_count() == 1U);
  CHECK(owner.reconciliation_view_at(0U).ingress_attempt() == attempt);
  const auto terminal =
      executor.reconciliation_admission_observation(admitted.value().attempt_ordinal);
  REQUIRE(terminal.has_value());
  CHECK(terminal->state == runtime::CriticalPrivateAdmissionState::EconomicallyConsumed);
  CHECK(terminal->disposition == oms::PrivateEventDisposition::Applied);
  CHECK_FALSE(executor.queue_snapshot().owner_bound);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A retained reconciliation capability becomes stale when its owner turn ends and then observes
// lease expiry after executor destruction without dereferencing the executor's former address.
TEST_CASE("retained reconciliation token rejects use after turn and executor lifetime",
          "[runtime][m4][reconciliation-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_reconciliation_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  ReconciliationAdmissionOwner owner;
  owner.retain_next_reconciliation_token();
  model::DeterministicClockProvider clock{70U};

  // ++++++++++++++++++++++++++++++++++++++++
  // Move the sole capability into owner storage after a successful in-turn inspection.
  {
    runtime::SerializedExecutor executor{0U, clock,
                                         create_admission_configuration_or_throw(authority), owner};
    runtime::DeterministicExecutorDriver driver{executor};
    const auto attempt = create_reconciliation_attempt_or_throw(fixture, authority, 8U, 0x68U);
    REQUIRE(executor.try_admit_reconciliation_event(attempt));
    REQUIRE(driver.bind_to_current_thread());
    const auto turn = driver.execute_next_turn();
    REQUIRE(turn);
    REQUIRE(turn.value().has_value());
    REQUIRE(owner.retained_reconciliation_token().has_value());
    const auto stale =
        owner.retained_reconciliation_token()->inspect_admitted_reconciliation_event_slot();
    REQUIRE_FALSE(stale);
    CHECK(stale.error() ==
          model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                              "reconciliation_admission.token_turn"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The same token consults its expired construction-time lease before its raw owner address.
  const auto expired =
      owner.retained_reconciliation_token()->inspect_admitted_reconciliation_event_slot();
  REQUIRE_FALSE(expired);
  CHECK(expired.error() ==
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "reconciliation_admission.expired_token"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
