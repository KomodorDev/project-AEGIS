// Purpose: independently prove M4 private-admission value assignments, sealed configuration,
// owner/capability authority, and the bounded lifecycle contract exercised by executor tests.

#include "aegis/runtime/private_order_admission.hpp"
#include "aegis/runtime/serialized_executor.hpp"
#include "m4_private_event_fixture.hpp"
#include "m4_test_authority.hpp"
#include "reference_configuration.hpp"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// Public ingress accepts only a stable const source value; the deleted rvalue path preserves
// producer ownership when a capacity or fence decision does not consume the attempt.
template <typename Executor, typename Attempt>
concept AdmitsPrivateConstReference = requires(Executor& executor, const Attempt& attempt) {
  {
    executor.try_admit_private(attempt)
  } -> std::same_as<model::Result<runtime::PrivateAdmissionDecision>>;
};

template <typename Executor, typename Attempt>
concept AdmitsPrivateMutableRvalue = requires(Executor& executor, Attempt attempt) {
  executor.try_admit_private(std::move(attempt));
};

template <typename Executor, typename Attempt>
concept AdmitsPrivateConstRvalue = requires(Executor& executor, const Attempt attempt) {
  executor.try_admit_private(std::move(attempt));
};

// ########################################################################

// ########################################################################
// Literal assignments and construction traits are authored test expectations, not reflections of
// a production lookup table or declaration order.
static_assert(static_cast<std::uint8_t>(runtime::AdmissionOutcome::Accepted) == 1U);
static_assert(static_cast<std::uint8_t>(runtime::AdmissionOutcome::CapacityExceeded) == 2U);
static_assert(static_cast<std::uint8_t>(runtime::AdmissionOutcome::Closed) == 3U);
static_assert(static_cast<std::uint8_t>(runtime::TurnKind::Command) == 1U);
static_assert(static_cast<std::uint8_t>(runtime::TurnKind::SourceDiscontinuity) == 2U);
static_assert(static_cast<std::uint8_t>(runtime::TurnKind::PrivateCommand) == 3U);
static_assert(static_cast<std::uint8_t>(runtime::TurnKind::AccountSafetyFence) == 4U);
static_assert(static_cast<std::uint8_t>(runtime::TurnKind::GlobalPrivateFence) == 5U);
static_assert(
    static_cast<std::uint8_t>(runtime::CriticalPrivateAdmissionState::CopiedAndAdmitted) == 1U);
static_assert(
    static_cast<std::uint8_t>(runtime::CriticalPrivateAdmissionState::EconomicallyConsumed) == 2U);
static_assert(static_cast<std::uint8_t>(
                  runtime::CriticalPrivateAdmissionState::RetainedForReconciliation) == 3U);
static_assert(!std::default_initializable<runtime::PrivateAdmissionConfiguration>);
static_assert(!std::is_aggregate_v<runtime::PrivateAdmissionConfiguration>);
static_assert(!std::constructible_from<runtime::PrivateAdmissionConfiguration, std::uint32_t,
                                       std::uint32_t, std::uint32_t>);
static_assert(
    !std::constructible_from<runtime::PrivateAdmissionConfiguration, std::uint32_t, std::uint32_t,
                             std::uint32_t, std::vector<runtime::PrivateAdmissionAccountBinding>>);
static_assert(!std::default_initializable<runtime::AdmittedPrivateOrderSlot>);
static_assert(!std::copy_constructible<runtime::AdmittedPrivateOrderSlot>);
static_assert(!std::is_copy_assignable_v<runtime::AdmittedPrivateOrderSlot>);
static_assert(std::move_constructible<runtime::AdmittedPrivateOrderSlot>);
static_assert(!std::is_move_assignable_v<runtime::AdmittedPrivateOrderSlot>);
static_assert(!std::default_initializable<runtime::AdmittedPrivateOrderSlotView>);
static_assert(std::is_abstract_v<runtime::PrivateAdmissionOwner>);
static_assert(
    AdmitsPrivateConstReference<runtime::SerializedExecutor, oms::PrivateOrderIngressAttempt>);
static_assert(
    !AdmitsPrivateMutableRvalue<runtime::SerializedExecutor, oms::PrivateOrderIngressAttempt>);
static_assert(
    !AdmitsPrivateConstRvalue<runtime::SerializedExecutor, oms::PrivateOrderIngressAttempt>);
static_assert(
    !noexcept(std::declval<const runtime::SerializedExecutor&>().private_admission_observation(
        std::declval<model::AdmissionOrdinal>())));

// ########################################################################

// ########################################################################
// A test-owned owner records only bounded copies exposed through the lawful capability API and can
// independently select consumption, successful buffering, configured-account retention, or global
// retention.
class TestPrivateAdmissionOwner final : public runtime::PrivateAdmissionOwner {
public:
  static constexpr std::size_t history_capacity = 64U;

  // ########################################################################
  // Closed test behavior selects one exact completion arm without production-derived expectations.
  enum class Mode : std::uint8_t {
    Consume = 1,
    Buffer = 2,
    RetainAccount = 3,
    RetainGlobal = 4,
  };

  // ########################################################################

  // --------------------------------------------------------
  // Select the next completion and independently control its committed evidence answer.
  void select_private_turn_completion(
      Mode mode, oms::PrivateEventDisposition completion_disposition,
      std::optional<oms::PrivateEventDisposition> committed_disposition) noexcept {
    mode_ = mode;
    completion_disposition_ = completion_disposition;
    committed_disposition_ = committed_disposition;
  }

  // --------------------------------------------------------
  // Select one assigned retained-account reason independently from producer capacity loss.
  void select_account_retention_reason(risk::AccountSafetyReason reason) noexcept {
    retention_reason_ = reason;
  }

  // --------------------------------------------------------
  // Advance one already buffered evidence row to its sole lawful later terminal disposition.
  [[nodiscard]] bool
  transition_buffered_disposition_to_applied(model::AdmissionOrdinal attempt_ordinal) noexcept {
    std::lock_guard lock{evidence_mutex_};
    for (std::size_t index = 0U; index < disposition_evidence_count_; ++index) {
      if (disposition_evidence_ordinals_[index] == attempt_ordinal &&
          disposition_evidence_[index] == oms::PrivateEventDisposition::BufferedGap) {
        disposition_evidence_[index].emplace(oms::PrivateEventDisposition::AppliedFromBuffer);
        return true;
      }
    }
    return false;
  }

  // --------------------------------------------------------
  // Observe the admitted slot through public synchronized queries while its owner turn is active.
  void request_in_flight_observation_from(runtime::SerializedExecutor& executor) noexcept {
    observed_executor_ = &executor;
  }

  // --------------------------------------------------------
  // Inject one later same-account attempt while the current account fence is owner-in-flight.
  void admit_during_next_account_fence(runtime::SerializedExecutor& executor,
                                       const oms::PrivateOrderIngressAttempt& attempt) noexcept {
    fence_race_executor_ = &executor;
    fence_race_attempt_ = &attempt;
  }

  // --------------------------------------------------------
  // Inject one producer attempt while an admitted private token is owner-in-flight.
  void admit_during_next_private_turn(runtime::SerializedExecutor& executor,
                                      const oms::PrivateOrderIngressAttempt& attempt) noexcept {
    private_race_executor_ = &executor;
    private_race_attempt_ = &attempt;
  }

  // --------------------------------------------------------
  // Retain the lawful immutable view, then return one test-authored closed owner-turn result.
  [[nodiscard]] runtime::PrivateTurnCompletion
  commit_private_order_turn(runtime::AdmittedPrivateOrderSlot admitted) noexcept override {
    auto inspected = admitted.inspect_admitted_private_order_slot();
    if (!inspected) {
      return runtime::RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
          inspected.error());
    }
    if (view_count_ == history_capacity) {
      return runtime::RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
          retention_error());
    }
    views_[view_count_].emplace(std::move(inspected).value());
    const auto attempt_ordinal = views_[view_count_]->admission_receipt().attempt_ordinal;
    ++view_count_;
    if (observed_executor_ != nullptr) {
      try {
        in_flight_observation_ = observed_executor_->private_admission_observation(
            views_[view_count_ - 1U]->admission_receipt().attempt_ordinal);
        in_flight_snapshot_.emplace(observed_executor_->private_lane_snapshot());
      } catch (...) {
        return runtime::RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
            retention_error());
      }
    }
    if (private_race_executor_ != nullptr && private_race_attempt_ != nullptr) {
      auto race_admission = private_race_executor_->try_admit_private(*private_race_attempt_);
      if (race_admission) {
        private_race_decision_.emplace(race_admission.value());
      } else {
        private_race_error_.emplace(race_admission.error());
      }
      private_race_executor_ = nullptr;
      private_race_attempt_ = nullptr;
    }
    if (retain_token_) {
      retained_token_.emplace(std::move(admitted));
    }
    switch (mode_) {
    case Mode::Consume:
      if (committed_disposition_.has_value()) {
        publish_committed_disposition(attempt_ordinal, committed_disposition_.value());
      }
      return runtime::ConsumedPrivateTurn{completion_disposition_};
    case Mode::Buffer:
      if (committed_disposition_.has_value()) {
        publish_committed_disposition(attempt_ordinal, committed_disposition_.value());
      }
      return runtime::BufferedPrivateTurn{};
    case Mode::RetainAccount: {
      auto error = retention_error();
      publish_committed_retention_error(attempt_ordinal, error);
      return runtime::RetainedPrivateTurn::create_retained_private_turn_for_account(
          std::move(error), retention_reason_);
    }
    case Mode::RetainGlobal: {
      auto error = retention_error();
      publish_committed_retention_error(attempt_ordinal, error);
      return runtime::RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
          std::move(error));
    }
    }
    return runtime::RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
        retention_error());
  }

  // --------------------------------------------------------
  // Answer only for the exact inspected admission ordinal, including its one lawful buffered
  // advance.
  [[nodiscard]] std::optional<oms::PrivateEventDisposition>
  find_committed_private_event_disposition(
      model::AdmissionOrdinal attempt_ordinal) const noexcept override {
    std::lock_guard lock{evidence_mutex_};
    for (std::size_t index = 0U; index < disposition_evidence_count_; ++index) {
      if (disposition_evidence_ordinals_[index] == attempt_ordinal) {
        return disposition_evidence_[index];
      }
    }
    return std::nullopt;
  }

  // --------------------------------------------------------
  // Find one immutable retained-error value in stable fixed storage synchronized with publication.
  [[nodiscard]] const model::DomainError* find_committed_retained_private_event_error(
      model::AdmissionOrdinal attempt_ordinal) const noexcept override {
    std::lock_guard lock{evidence_mutex_};
    for (std::size_t index = 0U; index < retention_evidence_count_; ++index) {
      if (retention_evidence_ordinals_[index] == attempt_ordinal) {
        return &retention_evidence_[index].value();
      }
    }
    return nullptr;
  }

  // --------------------------------------------------------
  // Copy the exact configured-account fence and context before reporting success or an authored
  // owner failure.
  [[nodiscard]] model::Result<void>
  apply_account_safety_fence(const runtime::AccountSafetyFenceTurn& fence,
                             const runtime::ControlTurnContext& context) noexcept override {
    if (account_fence_count_ == fence_history_capacity) {
      return model::Result<void>::create_failure(retention_error());
    }
    account_fences_[account_fence_count_].emplace(fence);
    account_fence_contexts_[account_fence_count_].emplace(context);
    ++account_fence_count_;
    if (fence_race_executor_ != nullptr && fence_race_attempt_ != nullptr) {
      auto admitted = fence_race_executor_->try_admit_private(*fence_race_attempt_);
      if (admitted) {
        fence_race_decision_.emplace(admitted.value());
      } else {
        fence_race_error_.emplace(admitted.error());
      }
      fence_race_executor_ = nullptr;
      fence_race_attempt_ = nullptr;
    }
    if (should_fail_next_account_fence_) {
      should_fail_next_account_fence_ = false;
      return model::Result<void>::create_failure(retention_error());
    }
    return model::Result<void>::create_success();
  }

  // --------------------------------------------------------
  // Copy the reasonless global fence and context without selecting an account in the test owner.
  [[nodiscard]] model::Result<void>
  apply_global_private_fence(const runtime::GlobalPrivateFenceTurn& fence,
                             const runtime::ControlTurnContext& context) noexcept override {
    global_fence_.emplace(fence);
    global_fence_context_.emplace(context);
    return model::Result<void>::create_success();
  }

  // --------------------------------------------------------
  // Borrow the most recently observed slot view, or an empty optional before the first observation.
  [[nodiscard]] const std::optional<runtime::AdmittedPrivateOrderSlotView>&
  latest_slot_view() const noexcept {
    if (view_count_ == 0U) {
      return empty_view_;
    }
    return views_[view_count_ - 1U];
  }

  // --------------------------------------------------------
  // Return the number of slot views retained in bounded observation order.
  [[nodiscard]] std::size_t slot_view_count() const noexcept { return view_count_; }

  // --------------------------------------------------------
  // Borrow the retained slot view at a caller-validated history index.
  [[nodiscard]] const runtime::AdmittedPrivateOrderSlotView&
  slot_view_at(std::size_t index) const noexcept {
    return views_[index].value();
  }

  // --------------------------------------------------------
  // Retain a moved token only to prove that public inspection fails after its owner turn ends.
  void retain_next_token(bool retain = true) noexcept { retain_token_ = retain; }

  // --------------------------------------------------------
  // Borrow the optionally retained owner token used to test post-turn invalidation.
  [[nodiscard]] const std::optional<runtime::AdmittedPrivateOrderSlot>&
  retained_token() const noexcept {
    return retained_token_;
  }

  // --------------------------------------------------------
  // Borrow the observation captured while the admitted slot remained owner-in-flight.
  [[nodiscard]] const std::optional<runtime::PrivateAdmissionObservation>&
  in_flight_observation() const noexcept {
    return in_flight_observation_;
  }

  // --------------------------------------------------------
  // Borrow the lane snapshot captured during the same owner-in-flight observation.
  [[nodiscard]] const std::optional<runtime::PrivateLaneSnapshot>&
  in_flight_snapshot() const noexcept {
    return in_flight_snapshot_;
  }

  // --------------------------------------------------------
  // Borrow the most recently delivered account fence, or an empty optional before delivery.
  [[nodiscard]] const std::optional<runtime::AccountSafetyFenceTurn>&
  latest_account_fence() const noexcept {
    if (account_fence_count_ == 0U) {
      return empty_account_fence_;
    }
    return account_fences_[account_fence_count_ - 1U];
  }

  // --------------------------------------------------------
  // Return the number of account fences retained in bounded delivery order.
  [[nodiscard]] std::size_t account_fence_count() const noexcept { return account_fence_count_; }

  // --------------------------------------------------------
  // Borrow the retained account fence at a caller-validated history index.
  [[nodiscard]] const runtime::AccountSafetyFenceTurn&
  account_fence_at(std::size_t index) const noexcept {
    return account_fences_[index].value();
  }

  // --------------------------------------------------------
  // Borrow the reasonless global fence delivered to this test owner, when present.
  [[nodiscard]] const std::optional<runtime::GlobalPrivateFenceTurn>&
  global_fence() const noexcept {
    return global_fence_;
  }

  // --------------------------------------------------------
  // Borrow the control-turn context paired with the most recent account fence.
  [[nodiscard]] const std::optional<runtime::ControlTurnContext>&
  account_fence_context() const noexcept {
    if (account_fence_count_ == 0U) {
      return empty_control_context_;
    }
    return account_fence_contexts_[account_fence_count_ - 1U];
  }

  // --------------------------------------------------------
  // Borrow the control-turn context paired with the delivered global fence.
  [[nodiscard]] const std::optional<runtime::ControlTurnContext>&
  global_fence_context() const noexcept {
    return global_fence_context_;
  }

  // --------------------------------------------------------
  // Make the next account-fence callback return the stable test error after recording its input.
  void request_next_account_fence_failure() noexcept { should_fail_next_account_fence_ = true; }

  // --------------------------------------------------------
  // Borrow the same-account admission decision observed during an account-fence race.
  [[nodiscard]] const std::optional<runtime::PrivateAdmissionDecision>&
  fence_race_decision() const noexcept {
    return fence_race_decision_;
  }

  // --------------------------------------------------------
  // Borrow the same-account admission error observed during an account-fence race.
  [[nodiscard]] const std::optional<model::DomainError>& fence_race_error() const noexcept {
    return fence_race_error_;
  }

  // --------------------------------------------------------
  // Borrow the admission decision observed during an owner-in-flight private-lane race.
  [[nodiscard]] const std::optional<runtime::PrivateAdmissionDecision>&
  private_race_decision() const noexcept {
    return private_race_decision_;
  }

  // --------------------------------------------------------
  // Borrow the admission error observed during an owner-in-flight private-lane race.
  [[nodiscard]] const std::optional<model::DomainError>& private_race_error() const noexcept {
    return private_race_error_;
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish one initial disposition in bounded storage before its matching owner completion
  // returns.
  void publish_committed_disposition(model::AdmissionOrdinal attempt_ordinal,
                                     oms::PrivateEventDisposition disposition) noexcept {
    std::lock_guard lock{evidence_mutex_};
    if (disposition_evidence_count_ == history_capacity) {
      return;
    }
    disposition_evidence_ordinals_[disposition_evidence_count_].emplace(attempt_ordinal);
    disposition_evidence_[disposition_evidence_count_].emplace(disposition);
    ++disposition_evidence_count_;
  }

  // --------------------------------------------------------
  // Publish one immutable retained error in stable storage before its completion leaves the owner.
  void publish_committed_retention_error(model::AdmissionOrdinal attempt_ordinal,
                                         const model::DomainError& error) noexcept {
    std::lock_guard lock{evidence_mutex_};
    if (retention_evidence_count_ == history_capacity) {
      return;
    }
    retention_evidence_ordinals_[retention_evidence_count_].emplace(attempt_ordinal);
    retention_evidence_[retention_evidence_count_].emplace(error);
    ++retention_evidence_count_;
  }

  // --------------------------------------------------------
  // Use one stable nominal error for every authored retention branch.
  [[nodiscard]] static model::DomainError retention_error() {
    return model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                               "test_private_retention");
  }

  // --------------------------------------------------------
  Mode mode_{Mode::Consume};
  oms::PrivateEventDisposition completion_disposition_{oms::PrivateEventDisposition::Applied};
  std::optional<oms::PrivateEventDisposition> committed_disposition_{
      oms::PrivateEventDisposition::Applied};
  risk::AccountSafetyReason retention_reason_{risk::AccountSafetyReason::CriticalAdmissionLoss};
  std::array<std::optional<runtime::AdmittedPrivateOrderSlotView>, history_capacity> views_{};
  mutable std::mutex evidence_mutex_;
  std::array<std::optional<model::AdmissionOrdinal>, history_capacity>
      disposition_evidence_ordinals_{};
  std::array<std::optional<oms::PrivateEventDisposition>, history_capacity> disposition_evidence_{};
  std::array<std::optional<model::AdmissionOrdinal>, history_capacity>
      retention_evidence_ordinals_{};
  std::array<std::optional<model::DomainError>, history_capacity> retention_evidence_{};
  std::size_t view_count_{0U};
  std::size_t disposition_evidence_count_{0U};
  std::size_t retention_evidence_count_{0U};
  bool retain_token_{false};
  std::optional<runtime::AdmittedPrivateOrderSlot> retained_token_;
  const std::optional<runtime::AdmittedPrivateOrderSlotView> empty_view_;
  runtime::SerializedExecutor* observed_executor_{nullptr};
  std::optional<runtime::PrivateAdmissionObservation> in_flight_observation_;
  std::optional<runtime::PrivateLaneSnapshot> in_flight_snapshot_;
  static constexpr std::size_t fence_history_capacity = 8U;
  std::array<std::optional<runtime::AccountSafetyFenceTurn>, fence_history_capacity>
      account_fences_{};
  std::array<std::optional<runtime::ControlTurnContext>, fence_history_capacity>
      account_fence_contexts_{};
  std::size_t account_fence_count_{0U};
  bool should_fail_next_account_fence_{false};
  const std::optional<runtime::AccountSafetyFenceTurn> empty_account_fence_;
  const std::optional<runtime::ControlTurnContext> empty_control_context_;
  runtime::SerializedExecutor* fence_race_executor_{nullptr};
  const oms::PrivateOrderIngressAttempt* fence_race_attempt_{nullptr};
  std::optional<runtime::PrivateAdmissionDecision> fence_race_decision_;
  std::optional<model::DomainError> fence_race_error_;
  runtime::SerializedExecutor* private_race_executor_{nullptr};
  const oms::PrivateOrderIngressAttempt* private_race_attempt_{nullptr};
  std::optional<runtime::PrivateAdmissionDecision> private_race_decision_;
  std::optional<model::DomainError> private_race_error_;
  std::optional<runtime::GlobalPrivateFenceTurn> global_fence_;
  std::optional<runtime::ControlTurnContext> global_fence_context_;
};

// ########################################################################

// ########################################################################
// One copied public command records its complete owner context so shared private/public counter
// and merge ordering can be proved without using production expectations.
struct PublicTurnRecord {
  std::optional<runtime::AcceptedTurnContext> context;
};

// ########################################################################

// ########################################################################
// The inline work value borrows bounded test state whose lifetime encloses the executor.
struct PublicRecordCommand {
  PublicTurnRecord* record;
};

// ########################################################################

// ########################################################################
// A fixed scripted clock exposes its exact observation count so pre-clock terminal boundaries and
// monotonicity failures can be proved without mutable production seams.
class ScriptedPrivateClock final : public model::ClockProvider {
public:

  // --------------------------------------------------------
  // Retain the fixed clock script that will be consumed in authored observation order.
  explicit ScriptedPrivateClock(std::array<std::uint64_t, 8U> values) noexcept : values_{values} {}

  // --------------------------------------------------------
  // Return the number of monotonic-clock observations made so far.
  [[nodiscard]] std::size_t observation_count() const noexcept { return observation_count_; }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Consume the authored prefix and repeat its final value only as a defensive test bound.
  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override {
    const auto index =
        observation_count_ < values_.size() ? observation_count_ : values_.size() - 1U;
    ++observation_count_;
    return values_[index];
  }

  // --------------------------------------------------------
  std::array<std::uint64_t, 8U> values_;
  std::size_t observation_count_{0U};
};

// ########################################################################

// --------------------------------------------------------
// Forward declarations let the independent test case use the helper contracts defined below.
[[nodiscard]] model::Result<void>
record_public_turn(const PublicRecordCommand& command,
                   const runtime::AcceptedTurnContext& context) noexcept;
[[nodiscard]] runtime::PrivateAdmissionConfiguration
create_private_admission_configuration_or_throw(const test_support::M4TestAuthority& authority);
[[nodiscard]] oms::PrivateOrderIngressAttempt create_private_timeout_attempt_or_throw(
    const test_support::M4PrivateEventFixture& fixture,
    const test_support::M4TestAuthority& authority, std::uint64_t event_counter,
    const model::LogicalAccountId& account_id, const model::VenueId& venue_id);
[[nodiscard]] oms::PrivateOrderIngressAttempt
create_private_timeout_attempt_or_throw(const test_support::M4PrivateEventFixture& fixture,
                                        const test_support::M4TestAuthority& authority,
                                        std::uint64_t event_counter);
[[nodiscard]] oms::PrivateOrderIngressAttempt create_private_timeout_attempt_or_throw(
    const test_support::M4PrivateEventFixture& fixture, std::uint64_t event_counter,
    const model::LogicalAccountId& account_id, const model::VenueId& venue_id);
[[nodiscard]] oms::PrivateOrderIngressAttempt
create_private_timeout_attempt_or_throw(const test_support::M4PrivateEventFixture& fixture,
                                        std::uint64_t event_counter);

// --------------------------------------------------------

// --------------------------------------------------------
// Public and private reserves remain physically independent while sharing exact attempt, receive,
// turn, clock, observation, merge-order, evidence, and slot-generation authority.
TEST_CASE("private admission shares counters and reuses only evidenced slots",
          "[runtime][m4][private-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_private_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  TestPrivateAdmissionOwner owner;
  model::DeterministicClockProvider clock{10U};
  runtime::SerializedExecutor executor{
      1U, clock, create_private_admission_configuration_or_throw(authority), owner};
  runtime::DeterministicExecutorDriver driver{executor};
  owner.request_in_flight_observation_from(executor);
  owner.retain_next_token();
  PublicTurnRecord public_record;

  // ++++++++++++++++++++++++++++++++++++++++
  // One public and one private value coexist; each reserve then rejects only its own next value.
  const auto public_admission = executor.try_admit(
      runtime::InlineCommandWorkItem::create_inline_command_work_item<&record_public_turn>(
          PublicRecordCommand{&public_record}));
  REQUIRE(public_admission);
  REQUIRE(public_admission.value().receipt.has_value());
  REQUIRE(clock.advance_nanoseconds(2U));
  const auto first_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 1U);
  const auto private_admission = executor.try_admit_private(first_attempt);
  REQUIRE(private_admission);
  REQUIRE(private_admission.value().receipt.has_value());
  const auto public_full = executor.try_admit(
      runtime::InlineCommandWorkItem::create_inline_command_work_item<&record_public_turn>(
          PublicRecordCommand{&public_record}));
  REQUIRE(public_full);
  const auto second_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 2U);
  const auto private_full = executor.try_admit_private(second_attempt);
  REQUIRE(private_full);

  CHECK(public_admission.value().attempt_ordinal.value() == 1U);
  CHECK(public_admission.value().receipt->receive_sequence.value() == 1U);
  CHECK(private_admission.value() ==
        runtime::PrivateAdmissionDecision{
            runtime::AdmissionOutcome::Accepted,
            test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(2U), 1U, 1U,
            runtime::AdmissionReceipt{
                test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(2U),
                test_support::create_m4_ordinal_or_throw<model::ReceiveSequence>(2U),
                model::ReceiveTimestamp{12U}, 1U, 1U},
            false, runtime::CriticalPrivateAdmissionState::CopiedAndAdmitted});
  CHECK(public_full.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(public_full.value().attempt_ordinal.value() == 3U);
  CHECK_FALSE(public_full.value().receipt.has_value());
  CHECK(private_full.value() ==
        runtime::PrivateAdmissionDecision{
            runtime::AdmissionOutcome::CapacityExceeded,
            test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(4U), 1U, 1U,
            std::nullopt, true, std::nullopt});
  CHECK(executor.private_lane_snapshot() ==
        runtime::PrivateLaneSnapshot{1U, 1U, 0U, 1U, 1U, 0U, 2U, false, false, false});

  // ++++++++++++++++++++++++++++++++++++++++
  // The merge executes attempts one, two, and four; the rejected public attempt owns no turn.
  REQUIRE(driver.bind_to_current_thread());
  REQUIRE(clock.advance_nanoseconds(8U));
  const auto public_turn = driver.execute_next_turn();
  REQUIRE(public_turn);
  REQUIRE(public_turn.value().has_value());
  CHECK(public_turn.value()->kind == runtime::TurnKind::Command);
  CHECK(public_turn.value()->attempt_ordinal.value() == 1U);
  CHECK(public_turn.value()->turn_ordinal.value() == 1U);
  REQUIRE(public_record.context.has_value());
  CHECK(public_record.context->receipt.receive_sequence.value() == 1U);
  CHECK(public_record.context->processing_timestamp == model::ProcessingTimestamp{20U});
  CHECK(public_record.context->queue_age == model::ElapsedNanoseconds{10U});

  REQUIRE(clock.advance_nanoseconds(1U));
  const auto private_turn = driver.execute_next_turn();
  REQUIRE(private_turn);
  REQUIRE(private_turn.value().has_value());
  CHECK(private_turn.value()->kind == runtime::TurnKind::PrivateCommand);
  CHECK(private_turn.value()->attempt_ordinal.value() == 2U);
  CHECK(private_turn.value()->turn_ordinal.value() == 2U);
  REQUIRE(owner.latest_slot_view().has_value());
  CHECK(owner.latest_slot_view()->admission_receipt() == private_admission.value().receipt.value());
  CHECK(owner.latest_slot_view()->processing_timestamp() == model::ProcessingTimestamp{21U});
  CHECK(owner.latest_slot_view()->queue_age() == model::ElapsedNanoseconds{9U});
  REQUIRE(owner.in_flight_observation().has_value());
  CHECK(owner.in_flight_observation()->state ==
        runtime::CriticalPrivateAdmissionState::CopiedAndAdmitted);
  REQUIRE(owner.in_flight_snapshot().has_value());
  CHECK(owner.in_flight_snapshot()->in_flight_slots == 1U);
  CHECK(owner.in_flight_snapshot()->occupied_slots == 1U);
  const auto first_terminal =
      executor.private_admission_observation(private_admission.value().attempt_ordinal);
  REQUIRE(first_terminal.has_value());
  CHECK(first_terminal->state == runtime::CriticalPrivateAdmissionState::EconomicallyConsumed);
  CHECK(first_terminal->disposition == oms::PrivateEventDisposition::Applied);
  REQUIRE(owner.retained_token().has_value());
  const auto stale_after_turn = owner.retained_token()->inspect_admitted_private_order_slot();
  REQUIRE_FALSE(stale_after_turn);
  CHECK(stale_after_turn.error() ==
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "private_admission.token_turn"));

  // ++++++++++++++++++++++++++++++++++++++++
  // The folded account fence follows its earliest rejected attempt and contains the first fact.
  REQUIRE(clock.advance_nanoseconds(1U));
  const auto fence_turn = driver.execute_next_turn();
  REQUIRE(fence_turn);
  REQUIRE(fence_turn.value().has_value());
  CHECK(fence_turn.value()->kind == runtime::TurnKind::AccountSafetyFence);
  CHECK(fence_turn.value()->attempt_ordinal.value() == 4U);
  CHECK(fence_turn.value()->turn_ordinal.value() == 3U);
  REQUIRE(owner.latest_account_fence().has_value());
  CHECK(owner.latest_account_fence()->lost_attempt_count == 1U);
  REQUIRE(owner.latest_account_fence()->reason_occurrence_count == 1U);
  REQUIRE(owner.latest_account_fence()->ordered_unique_reason_occurrences[0U].has_value());
  const auto& loss_occurrence =
      owner.latest_account_fence()->ordered_unique_reason_occurrences[0U].value();
  CHECK(loss_occurrence.first_attempt == second_attempt);
  CHECK(loss_occurrence.first_attempt_ordinal.value() == 4U);
  CHECK(loss_occurrence.reason == risk::AccountSafetyReason::CriticalAdmissionLoss);
  CHECK(executor.private_lane_snapshot() ==
        runtime::PrivateLaneSnapshot{0U, 0U, 0U, 1U, 0U, 0U, 2U, false, false, false});

  // ++++++++++++++++++++++++++++++++++++++++
  // Reusing the only physical slot cannot erase old evidence or revive its stale capability.
  owner.retain_next_token(false);
  const auto third_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 3U);
  const auto reused = executor.try_admit_private(third_attempt);
  REQUIRE(reused);
  REQUIRE(reused.value().receipt.has_value());
  CHECK(reused.value().attempt_ordinal.value() == 5U);
  CHECK(reused.value().receipt->receive_sequence.value() == 3U);
  const auto reused_turn = driver.execute_next_turn();
  REQUIRE(reused_turn);
  REQUIRE(reused_turn.value().has_value());
  CHECK(reused_turn.value()->turn_ordinal.value() == 4U);
  CHECK(owner.slot_view_count() == 2U);
  REQUIRE(executor.private_admission_observation(private_admission.value().attempt_ordinal)
              .has_value());
  const auto stale_after_reuse = owner.retained_token()->inspect_admitted_private_order_slot();
  REQUIRE_FALSE(stale_after_reuse);
  CHECK(stale_after_reuse.error() ==
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "private_admission.token_turn"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Filling the private reserve cannot consume or reduce the independent public command reserve.
TEST_CASE("private capacity cannot spill into the public reserve",
          "[runtime][m4][private-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_private_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  TestPrivateAdmissionOwner owner;
  model::DeterministicClockProvider clock{30U};
  runtime::SerializedExecutor executor{
      1U, clock, create_private_admission_configuration_or_throw(authority), owner};
  PublicTurnRecord public_record;
  const auto attempt = create_private_timeout_attempt_or_throw(fixture, authority, 6U);
  const auto private_admission = executor.try_admit_private(attempt);
  const auto public_admission = executor.try_admit(
      runtime::InlineCommandWorkItem::create_inline_command_work_item<&record_public_turn>(
          PublicRecordCommand{&public_record}));

  REQUIRE(private_admission);
  REQUIRE(public_admission);
  CHECK(private_admission.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(private_admission.value().pending_capacity == 1U);
  CHECK(public_admission.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(public_admission.value().pending_capacity == 1U);
  CHECK(executor.queue_snapshot().pending_commands == 1U);
  CHECK(executor.private_lane_snapshot().occupied_slots == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Record exactly one public owner context without allocating or invoking another executor seam.
[[nodiscard]] model::Result<void>
record_public_turn(const PublicRecordCommand& command,
                   const runtime::AcceptedTurnContext& context) noexcept {
  command.record->context.emplace(context);
  return model::Result<void>::create_success();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Extract the sealed admission configuration or fail immediately for a broken shared fixture.
[[nodiscard]] runtime::PrivateAdmissionConfiguration
create_private_admission_configuration_or_throw(const test_support::M4TestAuthority& authority) {
  auto created = runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
      authority.configuration, authority.m4_policy);
  if (!created) {
    throw std::logic_error{"invalid private-admission test configuration"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Mint one genuine account observation under the exact supplied M4 root and source pair.
[[nodiscard]] oms::PrivateOrderIngressAttempt create_private_timeout_attempt_or_throw(
    const test_support::M4PrivateEventFixture& fixture,
    const test_support::M4TestAuthority& authority, std::uint64_t event_counter,
    const model::LogicalAccountId& account_id, const model::VenueId& venue_id) {
  auto resolver = runtime::M4ProvenanceResolver::create_m4_provenance_resolver(
      authority.configuration, authority.m4_policy);
  if (!resolver) {
    throw std::logic_error{"invalid private-admission provenance resolver"};
  }
  const runtime::PrivateOrderEventFactory factory{std::move(resolver).value()};
  const auto origin =
      fixture.create_local_private_event_origin_or_throw(event_counter, 100U + event_counter, 999U);
  auto created = factory.create_account_timeout_attempt(
      oms::LocalPrivateIngressOrigin{origin.event_id, origin.source_time}, account_id, venue_id);
  if (!created) {
    throw std::logic_error{"invalid private-admission timeout attempt"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Use the fixture account pair while deriving provenance from the exact supplied M4 authority.
[[nodiscard]] oms::PrivateOrderIngressAttempt
create_private_timeout_attempt_or_throw(const test_support::M4PrivateEventFixture& fixture,
                                        const test_support::M4TestAuthority& authority,
                                        std::uint64_t event_counter) {
  return create_private_timeout_attempt_or_throw(fixture, authority, event_counter,
                                                 fixture.account_id(), fixture.venue_id());
}

// --------------------------------------------------------

// --------------------------------------------------------
// Mint one genuine receive-time-free account observation from the source-private factory boundary.
[[nodiscard]] oms::PrivateOrderIngressAttempt create_private_timeout_attempt_or_throw(
    const test_support::M4PrivateEventFixture& fixture, std::uint64_t event_counter,
    const model::LogicalAccountId& account_id, const model::VenueId& venue_id) {
  const auto origin =
      fixture.create_local_private_event_origin_or_throw(event_counter, 100U + event_counter, 999U);
  auto created = fixture.private_event_factory().create_account_timeout_attempt(
      oms::LocalPrivateIngressOrigin{origin.event_id, origin.source_time}, account_id, venue_id);
  if (!created) {
    throw std::logic_error{"invalid private-admission timeout attempt"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Use the baseline retained order's exact configured source pair for ordinary private admission.
[[nodiscard]] oms::PrivateOrderIngressAttempt
create_private_timeout_attempt_or_throw(const test_support::M4PrivateEventFixture& fixture,
                                        std::uint64_t event_counter) {
  return create_private_timeout_attempt_or_throw(fixture, event_counter, fixture.account_id(),
                                                 fixture.venue_id());
}

// --------------------------------------------------------

// --------------------------------------------------------
// Build a valid configuration whose fingerprint and organization revision cannot match the sealed
// policy under test.
[[nodiscard]] configuration::StartupConfiguration create_changed_startup_configuration_or_throw() {
  auto params = test_support::create_m3_enabled_two_firm_configuration_params_or_throw();
  auto next_revision = params.revision.derive_next_revision();
  if (!next_revision) {
    throw std::logic_error{"invalid changed private-admission configuration revision"};
  }
  params.revision = std::move(next_revision).value();
  auto changed =
      configuration::StartupConfiguration::create_startup_configuration(std::move(params));
  if (!changed) {
    throw std::logic_error{"invalid changed private-admission configuration"};
  }
  return std::move(changed).value();
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Sealed private-admission configuration copies exact policy capacities and canonical account
// bindings, while an unrelated startup configuration fails before publishing a value.
TEST_CASE("private admission configuration is sealed to matching M4 authority",
          "[runtime][m4][private-admission]") {
  const auto authority = test_support::create_m4_test_authority_or_throw();
  const auto created =
      runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
          authority.configuration, authority.m4_policy);
  REQUIRE(created);
  const auto& private_configuration = created.value();
  CHECK(private_configuration.private_admission_capacity() == 32U);
  CHECK(private_configuration.reconciliation_admission_capacity() == 32U);
  CHECK(private_configuration.account_safety_fence_capacity() == 32U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Account bindings retain startup canonical order and exactly the two attribution fields.
  const auto& expected = authority.configuration.logical_accounts();
  const auto& actual = private_configuration.account_bindings();
  REQUIRE(actual.size() == expected.size());
  for (std::size_t index = 0U; index < actual.size(); ++index) {
    CHECK(actual[index] == runtime::PrivateAdmissionAccountBinding{
                               expected[index].logical_account_id, expected[index].venue_id});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A different configuration cannot borrow the original policy's sealed capacity authority.
  const auto mismatched =
      runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
          create_changed_startup_configuration_or_throw(), authority.m4_policy);
  REQUIRE_FALSE(mismatched);
  CHECK(mismatched.error() ==
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidM4Policy,
                                            "private_admission.configuration_fingerprint"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Decision, observation, completion, and snapshot values retain their exact closed presence shapes
// without granting admission, owner-turn, or evidence authority.
TEST_CASE("private admission values preserve exact lifecycle presence",
          "[runtime][m4][private-admission]") {
  const auto receipt = runtime::AdmissionReceipt{
      test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(7U),
      test_support::create_m4_ordinal_or_throw<model::ReceiveSequence>(5U),
      model::ReceiveTimestamp{90U}, 2U, 3U};
  const auto accepted =
      runtime::PrivateAdmissionDecision{runtime::AdmissionOutcome::Accepted,
                                        receipt.attempt_ordinal,
                                        2U,
                                        3U,
                                        receipt,
                                        false,
                                        runtime::CriticalPrivateAdmissionState::CopiedAndAdmitted};
  CHECK(accepted.receipt == receipt);
  CHECK(accepted.state == runtime::CriticalPrivateAdmissionState::CopiedAndAdmitted);
  CHECK_FALSE(accepted.account_fence_recorded);

  // ++++++++++++++++++++++++++++++++++++++++
  // The three lifecycle observations use disposition and error presence exactly as assigned.
  const auto copied = runtime::PrivateAdmissionObservation{
      receipt.attempt_ordinal, runtime::CriticalPrivateAdmissionState::CopiedAndAdmitted,
      std::nullopt, std::nullopt};
  const auto consumed = runtime::PrivateAdmissionObservation{
      receipt.attempt_ordinal, runtime::CriticalPrivateAdmissionState::EconomicallyConsumed,
      oms::PrivateEventDisposition::Applied, std::nullopt};
  const auto retention_error = model::DomainError::create_at_field(
      model::DomainErrorCode::InvalidPrivateEvent, "test_private_retention");
  const auto retained = runtime::PrivateAdmissionObservation{
      receipt.attempt_ordinal, runtime::CriticalPrivateAdmissionState::RetainedForReconciliation,
      std::nullopt, retention_error};
  CHECK_FALSE(copied.disposition.has_value());
  CHECK_FALSE(copied.retention_error.has_value());
  CHECK(consumed.disposition == oms::PrivateEventDisposition::Applied);
  CHECK_FALSE(consumed.retention_error.has_value());
  CHECK_FALSE(retained.disposition.has_value());
  CHECK(retained.retention_error == retention_error);

  // ++++++++++++++++++++++++++++++++++++++++
  // Named retention factories keep configured-account reason presence distinct from global loss.
  const auto account = runtime::RetainedPrivateTurn::create_retained_private_turn_for_account(
      retention_error, risk::AccountSafetyReason::CriticalAdmissionLoss);
  const auto global =
      runtime::RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
          retention_error);
  CHECK(account.retention_error() == retention_error);
  CHECK(account.account_safety_reason() == risk::AccountSafetyReason::CriticalAdmissionLoss);
  CHECK_FALSE(account.normalized_input().has_value());
  CHECK_FALSE(account.first_admission_resolution().has_value());
  CHECK(global.retention_error() == retention_error);
  CHECK_FALSE(global.account_safety_reason().has_value());
  CHECK_FALSE(global.normalized_input().has_value());
  CHECK_FALSE(global.first_admission_resolution().has_value());
  CHECK(std::holds_alternative<runtime::ConsumedPrivateTurn>(runtime::PrivateTurnCompletion{
      runtime::ConsumedPrivateTurn{oms::PrivateEventDisposition::ProjectionOnly}}));
  CHECK(std::holds_alternative<runtime::BufferedPrivateTurn>(
      runtime::PrivateTurnCompletion{runtime::BufferedPrivateTurn{}}));
  CHECK(
      std::holds_alternative<runtime::RetainedPrivateTurn>(runtime::PrivateTurnCompletion{global}));

  // ++++++++++++++++++++++++++++++++++++++++
  // The diagnostic snapshot begins at the exact disabled/empty boundary without hidden capacity.
  CHECK(runtime::PrivateLaneSnapshot{} ==
        runtime::PrivateLaneSnapshot{0U, 0U, 0U, 0U, 0U, 0U, 0U, false, false, false});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Legacy M1-M3 constructors leave every M4 reserve and gate disabled, preserving the exact
// diagnostic value appended to existing queue, turn, and drive reports.
TEST_CASE("legacy executors expose a disabled private lane", "[runtime][m4][private-admission]") {
  model::DeterministicClockProvider clock{10U};
  runtime::SerializedExecutor executor{1U, clock};
  test_support::M4PrivateEventFixture fixture;
  const auto attempt = create_private_timeout_attempt_or_throw(fixture, 1U);
  const auto snapshot = executor.queue_snapshot();

  CHECK(snapshot.private_lane == runtime::PrivateLaneSnapshot{});
  CHECK(snapshot.pending_commands == 0U);
  CHECK(snapshot.pending_fences == 0U);
  CHECK_FALSE(snapshot.closed);
  CHECK_FALSE(snapshot.faulted);

  // ++++++++++++++++++++++++++++++++++++++++
  // Disabled private ingress creates no shared ordinal, receive observation, or hidden fence.
  const auto disabled = executor.try_admit_private(attempt);
  REQUIRE_FALSE(disabled);
  CHECK(disabled.error() ==
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "private_admission.disabled"));
  PublicTurnRecord public_record;
  const auto public_admission = executor.try_admit(
      runtime::InlineCommandWorkItem::create_inline_command_work_item<&record_public_turn>(
          PublicRecordCommand{&public_record}));
  REQUIRE(public_admission);
  REQUIRE(public_admission.value().receipt.has_value());
  CHECK(public_admission.value().attempt_ordinal.value() == 1U);
  CHECK(public_admission.value().receipt->receive_sequence.value() == 1U);
  CHECK(public_admission.value().receipt->received_at == model::ReceiveTimestamp{10U});
  CHECK(executor.private_admission_observation(model::AdmissionOrdinal::create_initial()) ==
        std::nullopt);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A valid owner-selected retention remains permanently addressable and activates exactly one
// configured-account containment turn without pretending that economics were consumed.
TEST_CASE("retained private turns stay observable and fence their exact account",
          "[runtime][m4][private-admission]") {
  const auto authority = test_support::create_m4_test_authority_or_throw();
  test_support::M4PrivateEventFixture fixture;
  TestPrivateAdmissionOwner owner;
  owner.select_private_turn_completion(TestPrivateAdmissionOwner::Mode::RetainAccount,
                                       oms::PrivateEventDisposition::Applied, std::nullopt);
  model::DeterministicClockProvider clock{40U};
  runtime::SerializedExecutor executor{
      0U, clock, create_private_admission_configuration_or_throw(authority), owner};
  runtime::DeterministicExecutorDriver driver{executor};
  const auto attempt = create_private_timeout_attempt_or_throw(fixture, 11U);
  const auto admitted = executor.try_admit_private(attempt);
  REQUIRE(admitted);
  REQUIRE(admitted.value().receipt.has_value());
  const auto queued = executor.private_admission_observation(admitted.value().attempt_ordinal);
  REQUIRE(queued.has_value());
  CHECK(queued->state == runtime::CriticalPrivateAdmissionState::CopiedAndAdmitted);

  // ++++++++++++++++++++++++++++++++++++++++
  // The private turn succeeds as a retained terminal lifecycle, not as a handler fault.
  REQUIRE(driver.bind_to_current_thread());
  const auto private_turn = driver.execute_next_turn();
  REQUIRE(private_turn);
  REQUIRE(private_turn.value().has_value());
  CHECK(private_turn.value()->kind == runtime::TurnKind::PrivateCommand);
  const auto retained = executor.private_admission_observation(admitted.value().attempt_ordinal);
  REQUIRE(retained.has_value());
  CHECK(retained->state == runtime::CriticalPrivateAdmissionState::RetainedForReconciliation);
  CHECK_FALSE(retained->disposition.has_value());
  CHECK(retained->retention_error ==
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                            "test_private_retention"));
  CHECK(executor.private_lane_snapshot() ==
        runtime::PrivateLaneSnapshot{0U, 0U, 0U, 32U, 1U, 0U, 2U, false, false, false});

  // ++++++++++++++++++++++++++++++++++++++++
  // Successful owner evidence clears the summary fence while append-only retained evidence remains.
  const auto fence_turn = driver.execute_next_turn();
  REQUIRE(fence_turn);
  REQUIRE(fence_turn.value().has_value());
  CHECK(fence_turn.value()->kind == runtime::TurnKind::AccountSafetyFence);
  CHECK(fence_turn.value()->attempt_ordinal == admitted.value().attempt_ordinal);
  REQUIRE(owner.latest_account_fence().has_value());
  CHECK(owner.latest_account_fence()->logical_account_id == fixture.account_id());
  CHECK(owner.latest_account_fence()->venue_id == fixture.venue_id());
  CHECK(owner.latest_account_fence()->lost_attempt_count == 1U);
  REQUIRE(owner.latest_account_fence()->reason_occurrence_count == 1U);
  REQUIRE(owner.latest_account_fence()->ordered_unique_reason_occurrences[0U].has_value());
  const auto& retained_occurrence =
      owner.latest_account_fence()->ordered_unique_reason_occurrences[0U].value();
  CHECK(retained_occurrence.first_attempt == attempt);
  CHECK(retained_occurrence.first_attempt_ordinal == admitted.value().attempt_ordinal);
  REQUIRE(owner.account_fence_context().has_value());
  CHECK(owner.account_fence_context()->turn_ordinal.value() == 2U);
  CHECK(owner.account_fence_context()->processing_timestamp == model::ProcessingTimestamp{40U});
  CHECK(executor.private_lane_snapshot().occupied_slots == 0U);
  CHECK(executor.private_lane_snapshot().pending_account_fences == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Repeated losses fold into one account interval. A producer race during its owner call becomes a
// successor interval, while the resulting exact-account gate does not block a configured peer.
TEST_CASE("account fences preserve producer races and isolate their configured account",
          "[runtime][m4][private-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_private_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  TestPrivateAdmissionOwner owner;
  model::DeterministicClockProvider clock{50U};
  runtime::SerializedExecutor executor{
      0U, clock, create_private_admission_configuration_or_throw(authority), owner};
  runtime::DeterministicExecutorDriver driver{executor};
  const auto accepted_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 21U);
  const auto first_lost_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 22U);
  const auto folded_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 23U);
  const auto successor_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 24U);

  REQUIRE(executor.try_admit_private(accepted_attempt));
  const auto first_loss = executor.try_admit_private(first_lost_attempt);
  const auto folded_loss = executor.try_admit_private(folded_attempt);
  REQUIRE(first_loss);
  REQUIRE(folded_loss);
  CHECK(first_loss.value().attempt_ordinal.value() == 2U);
  CHECK(folded_loss.value().attempt_ordinal.value() == 3U);
  CHECK(executor.private_lane_snapshot().pending_account_fences == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Consume the older slot, then inject one same-account loss while the folded interval is in
  // flight. Successful owner publication clears only the extracted interval.
  REQUIRE(driver.bind_to_current_thread());
  REQUIRE(driver.execute_next_turn());
  owner.admit_during_next_account_fence(executor, successor_attempt);
  const auto first_fence_turn = driver.execute_next_turn();
  REQUIRE(first_fence_turn);
  REQUIRE(first_fence_turn.value().has_value());
  REQUIRE(owner.account_fence_count() == 1U);
  CHECK(owner.account_fence_at(0U).lost_attempt_count == 2U);
  REQUIRE(owner.account_fence_at(0U).reason_occurrence_count == 1U);
  REQUIRE(owner.account_fence_at(0U).ordered_unique_reason_occurrences[0U].has_value());
  const auto& first_loss_occurrence =
      owner.account_fence_at(0U).ordered_unique_reason_occurrences[0U].value();
  CHECK(first_loss_occurrence.first_attempt == first_lost_attempt);
  CHECK(first_loss_occurrence.first_attempt_ordinal.value() == 2U);
  REQUIRE(owner.fence_race_decision().has_value());
  CHECK(owner.fence_race_decision()->outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(owner.fence_race_decision()->attempt_ordinal.value() == 4U);
  CHECK(owner.fence_race_decision()->account_fence_recorded);
  CHECK_FALSE(owner.fence_race_error().has_value());
  CHECK(executor.private_lane_snapshot().pending_account_fences == 1U);
  CHECK(executor.private_lane_snapshot().in_flight_account_fences == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The exact gated account folds another loss into its successor while a different configured
  // account can use the reusable private slot and remains outside that account-local gate.
  const auto later_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 25U);
  const auto& bindings = authority.configuration.logical_accounts();
  REQUIRE(bindings.size() == 2U);
  const auto peer_attempt = create_private_timeout_attempt_or_throw(
      fixture, authority, 26U, bindings[1U].logical_account_id, bindings[1U].venue_id);
  const auto peer = executor.try_admit_private(peer_attempt);
  REQUIRE(peer);
  CHECK(peer.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(peer.value().attempt_ordinal.value() == 5U);
  CHECK_FALSE(peer.value().account_fence_recorded);
  const auto gated = executor.try_admit_private(later_attempt);
  REQUIRE(gated);
  CHECK(gated.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(gated.value().attempt_ordinal.value() == 6U);
  CHECK(gated.value().account_fence_recorded);
  CHECK(executor.private_lane_snapshot().occupied_slots == 1U);
  CHECK(executor.private_lane_snapshot().pending_account_fences == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Global ordinal order delivers the successor interval before the peer private turn. The one
  // account slot preserves its earliest fact while counting both losses.
  const auto successor_turn = driver.execute_next_turn();
  REQUIRE(successor_turn);
  REQUIRE(successor_turn.value().has_value());
  CHECK(successor_turn.value()->attempt_ordinal.value() == 4U);
  REQUIRE(owner.account_fence_count() == 2U);
  CHECK(owner.account_fence_at(1U).lost_attempt_count == 2U);
  REQUIRE(owner.account_fence_at(1U).reason_occurrence_count == 1U);
  REQUIRE(owner.account_fence_at(1U).ordered_unique_reason_occurrences[0U].has_value());
  const auto& successor_loss_occurrence =
      owner.account_fence_at(1U).ordered_unique_reason_occurrences[0U].value();
  CHECK(successor_loss_occurrence.reason == risk::AccountSafetyReason::CriticalAdmissionLoss);
  CHECK(successor_loss_occurrence.first_attempt == successor_attempt);
  CHECK(successor_loss_occurrence.first_attempt_ordinal.value() == 4U);
  CHECK(executor.private_lane_snapshot().pending_account_fences == 0U);
  const auto peer_turn = driver.execute_next_turn();
  REQUIRE(peer_turn);
  REQUIRE(peer_turn.value().has_value());
  CHECK(peer_turn.value()->kind == runtime::TurnKind::PrivateCommand);
  CHECK(peer_turn.value()->attempt_ordinal.value() == 5U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A source fact with the same account/venue text but a foreign sealed M4 root cannot be attributed;
// it activates the sole reasonless global gate and is never silently consumed.
TEST_CASE("unattributable private loss activates and preserves the global gate",
          "[runtime][m4][private-admission]") {
  auto baseline_authority = test_support::create_m4_owner_test_authority_or_throw(
      test_support::create_reference_configuration_params_or_throw());
  auto configuration =
      runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
          baseline_authority.configuration, baseline_authority.m4_policy);
  REQUIRE(configuration);
  test_support::M4PrivateEventFixture fixture;
  const auto& executor_bindings = baseline_authority.configuration.logical_accounts();
  REQUIRE(executor_bindings.size() == 1U);
  CHECK(executor_bindings[0U].logical_account_id == fixture.account_id());
  CHECK(executor_bindings[0U].venue_id == fixture.venue_id());
  CHECK(configuration.value().root_provenance() !=
        fixture.test_authority().m4_policy.root_provenance());
  const auto unconfigured_attempt = create_private_timeout_attempt_or_throw(
      fixture, 31U, fixture.account_id(), fixture.venue_id());
  TestPrivateAdmissionOwner owner;
  model::DeterministicClockProvider clock{60U};
  runtime::SerializedExecutor executor{0U, clock, std::move(configuration).value(), owner};
  runtime::DeterministicExecutorDriver driver{executor};

  // ++++++++++++++++++++++++++++++++++++++++
  // Exact attribution failure keeps the source value outside the empty private reserve.
  const auto rejected = executor.try_admit_private(unconfigured_attempt);
  REQUIRE(rejected);
  CHECK(rejected.value() ==
        runtime::PrivateAdmissionDecision{runtime::AdmissionOutcome::CapacityExceeded,
                                          model::AdmissionOrdinal::create_initial(), 0U, 32U,
                                          std::nullopt, false, std::nullopt});
  CHECK_FALSE(executor.private_admission_observation(model::AdmissionOrdinal::create_initial())
                  .has_value());
  CHECK(executor.private_lane_snapshot().global_fence_active);
  CHECK_FALSE(executor.private_lane_snapshot().global_fence_owner_applied);
  CHECK(executor.private_lane_snapshot().occupied_slots == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The one control turn carries no invented account or receive identity and leaves a live
  // in-memory gate for this executor lifetime; M4 does not claim crash durability here.
  REQUIRE(driver.bind_to_current_thread());
  const auto global_turn = driver.execute_next_turn();
  REQUIRE(global_turn);
  REQUIRE(global_turn.value().has_value());
  CHECK(global_turn.value()->kind == runtime::TurnKind::GlobalPrivateFence);
  CHECK(global_turn.value()->attempt_ordinal.value() == 1U);
  CHECK_FALSE(global_turn.value()->receive_sequence.has_value());
  CHECK_FALSE(global_turn.value()->received_at.has_value());
  REQUIRE(owner.global_fence().has_value());
  CHECK(owner.global_fence()->first_attempt == unconfigured_attempt);
  CHECK(owner.global_fence()->earliest_attempt_ordinal.value() == 1U);
  CHECK(owner.global_fence()->lost_attempt_count == 1U);
  REQUIRE(owner.global_fence_context().has_value());
  CHECK(owner.global_fence_context()->turn_ordinal.value() == 1U);
  CHECK(owner.global_fence_context()->processing_timestamp == model::ProcessingTimestamp{60U});
  CHECK(executor.private_lane_snapshot().global_fence_active);
  CHECK(executor.private_lane_snapshot().global_fence_owner_applied);

  // ++++++++++++++++++++++++++++++++++++++++
  // Once applied, the global gate also rejects a normally configured source without rerunning it.
  const auto configured_attempt = create_private_timeout_attempt_or_throw(fixture, 32U);
  const auto gated = executor.try_admit_private(configured_attempt);
  REQUIRE(gated);
  CHECK(gated.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(gated.value().attempt_ordinal.value() == 2U);
  CHECK_FALSE(gated.value().account_fence_recorded);
  const auto no_repeat = driver.execute_next_turn();
  REQUIRE(no_repeat);
  CHECK_FALSE(no_repeat.value().has_value());
  CHECK(executor.private_lane_snapshot().global_fence_owner_applied);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Direct consumption requires matching terminal evidence. BufferedGap instead finishes successfully
// without claiming queue ownership or economics until its older ordinal reaches AppliedFromBuffer.
TEST_CASE("private completion distinguishes direct consumption from buffered application",
          "[runtime][m4][private-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_private_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;

  // ++++++++++++++++++++++++++++++++++++++++
  // Exercise one invalid consumption shape through the complete admission and owner-turn boundary.
  const auto prove_invalid_consumption_is_contained =
      [&](oms::PrivateEventDisposition completion_disposition,
          oms::PrivateEventDisposition committed_disposition, std::uint64_t event_counter) {
        TestPrivateAdmissionOwner owner;
        owner.select_private_turn_completion(TestPrivateAdmissionOwner::Mode::Consume,
                                             completion_disposition, committed_disposition);
        model::DeterministicClockProvider clock{70U};
        runtime::SerializedExecutor executor{
            0U, clock, create_private_admission_configuration_or_throw(authority), owner};
        runtime::DeterministicExecutorDriver driver{executor};
        const auto attempt =
            create_private_timeout_attempt_or_throw(fixture, authority, event_counter);
        const auto admitted = executor.try_admit_private(attempt);
        REQUIRE(admitted);
        REQUIRE(driver.bind_to_current_thread());

        // ++++++++++++++++++++++++++++++++++++++++
        // Invalid consumption publishes no terminal lifecycle fact, retains its source behind the
        // configured-account fence, and returns the constructor-reserved failure exactly.
        const auto failed_turn = driver.execute_next_turn();
        REQUIRE_FALSE(failed_turn);
        const auto expected = model::DomainError::create_at_field(
            model::DomainErrorCode::InvalidPrivateEvent,
            "private_admission.find_committed_private_event_disposition");
        CHECK(failed_turn.error() == expected);
        CHECK(executor.terminal_error() == expected);
        const auto invalid =
            executor.private_admission_observation(admitted.value().attempt_ordinal);
        REQUIRE(invalid.has_value());
        CHECK(invalid->state == runtime::CriticalPrivateAdmissionState::RetainedForReconciliation);
        CHECK_FALSE(invalid->disposition.has_value());
        CHECK(invalid->retention_error == expected);
        CHECK(executor.private_lane_snapshot().occupied_slots == 0U);
        CHECK(executor.private_lane_snapshot().pending_account_fences == 1U);
        CHECK(executor.queue_snapshot().faulted);
        CHECK(executor.queue_snapshot().closed);

        // ++++++++++++++++++++++++++++++++++++++++
        // Terminal failure has precedence over assigning a later attempt or returning Closed.
        const auto later_attempt =
            create_private_timeout_attempt_or_throw(fixture, authority, event_counter + 100U);
        const auto later = executor.try_admit_private(later_attempt);
        REQUIRE_FALSE(later);
        CHECK(later.error() == expected);

        // ++++++++++++++++++++++++++++++++++++++++
      };

  // ++++++++++++++++++++++++++++++++++++++++
  SECTION("a committed disposition different from the completion is rejected") {
    prove_invalid_consumption_is_contained(oms::PrivateEventDisposition::Applied,
                                           oms::PrivateEventDisposition::ProjectionOnly, 41U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  SECTION(
      "BufferedGap succeeds nonterminally and later AppliedFromBuffer consumes the old ordinal") {
    TestPrivateAdmissionOwner owner;
    owner.select_private_turn_completion(TestPrivateAdmissionOwner::Mode::Buffer,
                                         oms::PrivateEventDisposition::BufferedGap,
                                         oms::PrivateEventDisposition::BufferedGap);
    model::DeterministicClockProvider clock{71U};
    runtime::SerializedExecutor executor{
        0U, clock, create_private_admission_configuration_or_throw(authority), owner};
    runtime::DeterministicExecutorDriver driver{executor};
    const auto attempt = create_private_timeout_attempt_or_throw(fixture, authority, 42U);
    const auto admitted = executor.try_admit_private(attempt);
    REQUIRE(admitted);
    REQUIRE(driver.bind_to_current_thread());

    // ++++++++++++++++++++++++++++++++++++++++
    // Buffer commit finishes the owner turn without false consumption, containment, or faulting.
    const auto buffered_turn = driver.execute_next_turn();
    REQUIRE(buffered_turn);
    REQUIRE(buffered_turn.value().has_value());
    CHECK_FALSE(
        executor.private_admission_observation(admitted.value().attempt_ordinal).has_value());
    CHECK_FALSE(executor.terminal_error().has_value());
    CHECK(executor.private_lane_snapshot().pending_account_fences == 0U);

    // ++++++++++++++++++++++++++++++++++++++++
    // A later owner transaction may advance only that buffered row; observation then becomes the
    // terminal economic acknowledgement for the original admission ordinal.
    REQUIRE(owner.transition_buffered_disposition_to_applied(admitted.value().attempt_ordinal));
    const auto applied = executor.private_admission_observation(admitted.value().attempt_ordinal);
    REQUIRE(applied.has_value());
    CHECK(applied->state == runtime::CriticalPrivateAdmissionState::EconomicallyConsumed);
    CHECK(applied->disposition == oms::PrivateEventDisposition::AppliedFromBuffer);
    CHECK_FALSE(applied->retention_error.has_value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  SECTION("AppliedFromBuffer is invalid as the current admission's direct completion") {
    prove_invalid_consumption_is_contained(oms::PrivateEventDisposition::AppliedFromBuffer,
                                           oms::PrivateEventDisposition::AppliedFromBuffer, 43U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Constructor-owned completion errors publish exact observation, terminal, and returned values;
// owner fence failures instead restore their complete interval before fail-close publication.
TEST_CASE("private failures preserve exact containment and returned errors",
          "[runtime][m4][private-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_private_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;

  SECTION("invalid retained reason shape consumes all three reserved error destinations") {
    TestPrivateAdmissionOwner owner;
    owner.select_private_turn_completion(TestPrivateAdmissionOwner::Mode::RetainGlobal,
                                         oms::PrivateEventDisposition::Applied, std::nullopt);
    model::DeterministicClockProvider clock{75U};
    runtime::SerializedExecutor executor{
        0U, clock, create_private_admission_configuration_or_throw(authority), owner};
    runtime::DeterministicExecutorDriver driver{executor};
    const auto attempt = create_private_timeout_attempt_or_throw(fixture, authority, 45U);
    const auto admitted = executor.try_admit_private(attempt);
    REQUIRE(admitted);
    REQUIRE(driver.bind_to_current_thread());

    // ++++++++++++++++++++++++++++++++++++++++
    // A configured account cannot return the reasonless global completion shape. The returned,
    // terminal, and retained-slot errors remain equal after their separate reserved moves.
    const auto failed_turn = driver.execute_next_turn();
    REQUIRE_FALSE(failed_turn);
    const auto expected = model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidPrivateEvent, "private_admission.retained_completion");
    CHECK(failed_turn.error() == expected);
    CHECK(executor.terminal_error() == expected);
    const auto invalid = executor.private_admission_observation(admitted.value().attempt_ordinal);
    REQUIRE(invalid.has_value());
    CHECK(invalid->state == runtime::CriticalPrivateAdmissionState::RetainedForReconciliation);
    CHECK_FALSE(invalid->disposition.has_value());
    CHECK(invalid->retention_error == expected);
    CHECK(executor.private_lane_snapshot().occupied_slots == 0U);
    CHECK(executor.private_lane_snapshot().pending_account_fences == 1U);

    // ++++++++++++++++++++++++++++++++++++++++
  }

  SECTION("account fence owner failure merges and restores an in-flight successor") {
    TestPrivateAdmissionOwner owner;
    owner.select_private_turn_completion(TestPrivateAdmissionOwner::Mode::RetainAccount,
                                         oms::PrivateEventDisposition::Applied, std::nullopt);
    owner.select_account_retention_reason(risk::AccountSafetyReason::TimeoutObserved);
    model::DeterministicClockProvider clock{80U};
    runtime::SerializedExecutor executor{
        0U, clock, create_private_admission_configuration_or_throw(authority), owner};
    runtime::DeterministicExecutorDriver driver{executor};
    const auto retained_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 46U);
    const auto successor_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 47U);
    const auto admitted = executor.try_admit_private(retained_attempt);
    REQUIRE(admitted);
    REQUIRE(driver.bind_to_current_thread());
    const auto retained_turn = driver.execute_next_turn();
    REQUIRE(retained_turn);
    REQUIRE(retained_turn.value().has_value());

    // ++++++++++++++++++++++++++++++++++++++++
    // Extract the TimeoutObserved interval, inject a later CriticalAdmissionLoss into the refilled
    // sole slot, then force the owner callback to fail after both intervals exist.
    owner.admit_during_next_account_fence(executor, successor_attempt);
    owner.request_next_account_fence_failure();

    // ++++++++++++++++++++++++++++++++++++++++
    // Failure checked-merges the extracted and successor intervals before terminal publication.
    const auto failed_fence = driver.execute_next_turn();
    REQUIRE_FALSE(failed_fence);
    const auto expected = model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidPrivateEvent, "test_private_retention");
    CHECK(failed_fence.error() == expected);
    CHECK(executor.terminal_error() == expected);
    REQUIRE(owner.fence_race_decision().has_value());
    CHECK(owner.fence_race_decision()->outcome == runtime::AdmissionOutcome::CapacityExceeded);
    CHECK(owner.fence_race_decision()->attempt_ordinal.value() == 2U);
    CHECK(owner.fence_race_decision()->account_fence_recorded);
    CHECK_FALSE(owner.fence_race_error().has_value());
    CHECK(executor.private_lane_snapshot().in_flight_account_fences == 0U);
    CHECK(executor.private_lane_snapshot().pending_account_fences == 1U);

    // ++++++++++++++++++++++++++++++++++++++++
    // The bounded diagnostic copy proves the restored single slot retained both counts and unique
    // reasons in first-occurrence order without exposing either complete private fact.
    const auto restored =
        executor.find_account_safety_fence_snapshot(fixture.account_id(), fixture.venue_id());
    REQUIRE(restored.has_value());
    CHECK_FALSE(restored->in_flight);
    CHECK(restored->earliest_pending_attempt_ordinal == admitted.value().attempt_ordinal);
    CHECK(restored->pending_lost_attempt_count == 2U);
    REQUIRE(restored->reason_occurrence_count == 2U);
    REQUIRE(restored->ordered_unique_reason_occurrences[0U].has_value());
    REQUIRE(restored->ordered_unique_reason_occurrences[1U].has_value());
    CHECK(restored->ordered_unique_reason_occurrences[0U]->reason ==
          risk::AccountSafetyReason::TimeoutObserved);
    CHECK(restored->ordered_unique_reason_occurrences[0U]->first_attempt_ordinal.value() == 1U);
    CHECK(restored->ordered_unique_reason_occurrences[1U]->reason ==
          risk::AccountSafetyReason::CriticalAdmissionLoss);
    CHECK(restored->ordered_unique_reason_occurrences[1U]->first_attempt_ordinal.value() == 2U);

    // ++++++++++++++++++++++++++++++++++++++++
    // The owner received only the extracted earlier interval; restoration, not the handler, merged
    // the later successor after the handler reported failure.
    REQUIRE(owner.latest_account_fence().has_value());
    CHECK(owner.latest_account_fence()->lost_attempt_count == 1U);
    REQUIRE(owner.latest_account_fence()->reason_occurrence_count == 1U);
    REQUIRE(owner.latest_account_fence()->ordered_unique_reason_occurrences[0U].has_value());
    CHECK(owner.latest_account_fence()->ordered_unique_reason_occurrences[0U]->first_attempt ==
          retained_attempt);
    const auto later_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 48U);
    const auto later = executor.try_admit_private(later_attempt);
    REQUIRE_FALSE(later);
    CHECK(later.error() == expected);

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Closure, shared counter exhaustion, and both clock domains fail at their specified pre-mutation
// boundaries without inventing a private decision, observation, or safety fence.
TEST_CASE("private admission counter clock and close edges fail at exact boundaries",
          "[runtime][m4][private-admission]") {
  const auto authority = test_support::create_m4_test_authority_or_throw();
  test_support::M4PrivateEventFixture fixture;
  const auto attempt = create_private_timeout_attempt_or_throw(fixture, 51U);
  constexpr auto maximum_drive_turns = std::numeric_limits<std::size_t>::max();
  const auto maximum_admission = test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(
      std::numeric_limits<std::uint64_t>::max());
  const auto maximum_receive = test_support::create_m4_ordinal_or_throw<model::ReceiveSequence>(
      std::numeric_limits<std::uint64_t>::max());
  const auto maximum_turn = test_support::create_m4_ordinal_or_throw<model::TurnOrdinal>(
      std::numeric_limits<std::uint64_t>::max());

  // ++++++++++++++++++++++++++++++++++++++++
  SECTION("configured closure assigns only an attempt") {
    TestPrivateAdmissionOwner owner;
    ScriptedPrivateClock clock{{80U, 80U, 80U, 80U, 80U, 80U, 80U, 80U}};
    runtime::SerializedExecutor executor{
        0U, clock, create_private_admission_configuration_or_throw(authority), owner};
    executor.close();
    const auto closed = executor.try_admit_private(attempt);
    REQUIRE(closed);
    CHECK(closed.value() ==
          runtime::PrivateAdmissionDecision{runtime::AdmissionOutcome::Closed,
                                            model::AdmissionOrdinal::create_initial(), 0U, 32U,
                                            std::nullopt, false, std::nullopt});
    CHECK(clock.observation_count() == 0U);
    CHECK_FALSE(executor.private_admission_observation(model::AdmissionOrdinal::create_initial())
                    .has_value());
    CHECK(executor.private_lane_snapshot().occupied_slots == 0U);
    CHECK_FALSE(executor.private_lane_snapshot().global_fence_active);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  SECTION("attempt exhaustion precedes a decision") {
    TestPrivateAdmissionOwner owner;
    ScriptedPrivateClock clock{{81U, 81U, 81U, 81U, 81U, 81U, 81U, 81U}};
    runtime::SerializedExecutor executor{
        0U,
        clock,
        create_private_admission_configuration_or_throw(authority),
        owner,
        maximum_drive_turns,
        runtime::ExecutorCounterSeed{maximum_admission, std::nullopt, std::nullopt}};
    const auto exhausted = executor.try_admit_private(attempt);
    REQUIRE_FALSE(exhausted);
    CHECK(exhausted.error() ==
          model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                              "admission_ordinal"));
    CHECK(clock.observation_count() == 0U);
    CHECK(executor.private_lane_snapshot().occupied_slots == 0U);
    CHECK(executor.private_lane_snapshot().pending_account_fences == 0U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  SECTION("receive exhaustion precedes a copy and clock read") {
    TestPrivateAdmissionOwner owner;
    ScriptedPrivateClock clock{{82U, 82U, 82U, 82U, 82U, 82U, 82U, 82U}};
    runtime::SerializedExecutor executor{
        0U,
        clock,
        create_private_admission_configuration_or_throw(authority),
        owner,
        maximum_drive_turns,
        runtime::ExecutorCounterSeed{
            test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(41U), maximum_receive,
            std::nullopt}};
    const auto exhausted = executor.try_admit_private(attempt);
    REQUIRE_FALSE(exhausted);
    CHECK(exhausted.error() ==
          model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                              "receive_sequence"));
    CHECK(clock.observation_count() == 0U);
    CHECK_FALSE(executor
                    .private_admission_observation(
                        test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(42U))
                    .has_value());
    CHECK(executor.private_lane_snapshot().occupied_slots == 0U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  SECTION("receive clock regression preserves only the earlier accepted fact") {
    TestPrivateAdmissionOwner owner;
    ScriptedPrivateClock clock{{10U, 9U, 9U, 9U, 9U, 9U, 9U, 9U}};
    runtime::SerializedExecutor executor{
        0U, clock, create_private_admission_configuration_or_throw(authority), owner};
    const auto first = executor.try_admit_private(attempt);
    REQUIRE(first);
    const auto second_attempt = create_private_timeout_attempt_or_throw(fixture, 52U);
    const auto regressed = executor.try_admit_private(second_attempt);
    REQUIRE_FALSE(regressed);
    CHECK(regressed.error() ==
          model::DomainError::create_at_field(model::DomainErrorCode::ExecutorClockRegression,
                                              "private_admission_clock"));
    CHECK(clock.observation_count() == 2U);
    CHECK(executor.private_lane_snapshot().occupied_slots == 1U);
    CHECK(executor.private_lane_snapshot().queued_slots == 1U);
    CHECK(executor.private_lane_snapshot().pending_account_fences == 0U);
    CHECK_FALSE(executor
                    .private_admission_observation(
                        test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(2U))
                    .has_value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  SECTION("turn exhaustion preserves the queued fact without invoking its owner") {
    TestPrivateAdmissionOwner owner;
    ScriptedPrivateClock clock{{83U, 83U, 83U, 83U, 83U, 83U, 83U, 83U}};
    runtime::SerializedExecutor executor{
        0U,
        clock,
        create_private_admission_configuration_or_throw(authority),
        owner,
        maximum_drive_turns,
        runtime::ExecutorCounterSeed{std::nullopt, std::nullopt, maximum_turn}};
    runtime::DeterministicExecutorDriver driver{executor};
    const auto admitted = executor.try_admit_private(attempt);
    REQUIRE(admitted);
    REQUIRE(driver.bind_to_current_thread());
    const auto exhausted = driver.execute_next_turn();
    REQUIRE_FALSE(exhausted);
    CHECK(exhausted.error() ==
          model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                              "turn_ordinal"));
    CHECK(clock.observation_count() == 1U);
    CHECK(owner.slot_view_count() == 0U);
    CHECK(executor.private_lane_snapshot().queued_slots == 1U);
    const auto observation =
        executor.private_admission_observation(admitted.value().attempt_ordinal);
    REQUIRE(observation.has_value());
    CHECK(observation->state == runtime::CriticalPrivateAdmissionState::CopiedAndAdmitted);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  SECTION("processing clock regression preserves the queued fact") {
    TestPrivateAdmissionOwner owner;
    ScriptedPrivateClock clock{{10U, 9U, 9U, 9U, 9U, 9U, 9U, 9U}};
    runtime::SerializedExecutor executor{
        0U, clock, create_private_admission_configuration_or_throw(authority), owner};
    runtime::DeterministicExecutorDriver driver{executor};
    const auto admitted = executor.try_admit_private(attempt);
    REQUIRE(admitted);
    REQUIRE(driver.bind_to_current_thread());
    const auto regressed = driver.execute_next_turn();
    REQUIRE_FALSE(regressed);
    CHECK(regressed.error() ==
          model::DomainError::create_at_field(model::DomainErrorCode::ExecutorClockRegression,
                                              "executor_processing_clock"));
    CHECK(clock.observation_count() == 2U);
    CHECK(owner.slot_view_count() == 0U);
    CHECK(executor.private_lane_snapshot().queued_slots == 1U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A retained move-only capability consults its construction-time lease before touching the raw
// executor address, so executor destruction changes stale-turn failure into exact expiry failure.
TEST_CASE("retained private token rejects inspection after executor destruction",
          "[runtime][m4][private-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_private_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  TestPrivateAdmissionOwner owner;
  owner.retain_next_token();
  model::DeterministicClockProvider clock{90U};

  // ++++++++++++++++++++++++++++++++++++++++
  // Consume one slot while moving its sole token into the owner beyond the callback lifetime.
  {
    runtime::SerializedExecutor executor{
        0U, clock, create_private_admission_configuration_or_throw(authority), owner};
    runtime::DeterministicExecutorDriver driver{executor};
    const auto attempt = create_private_timeout_attempt_or_throw(fixture, authority, 61U);
    REQUIRE(executor.try_admit_private(attempt));
    REQUIRE(driver.bind_to_current_thread());
    const auto turn = driver.execute_next_turn();
    REQUIRE(turn);
    REQUIRE(turn.value().has_value());
    REQUIRE(owner.retained_token().has_value());
    const auto inactive = owner.retained_token()->inspect_admitted_private_order_slot();
    REQUIRE_FALSE(inactive);
    CHECK(inactive.error() ==
          model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                              "private_admission.token_turn"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The same retained value must reject through its expired lease without dereferencing storage.
  const auto expired = owner.retained_token()->inspect_admitted_private_order_slot();
  REQUIRE_FALSE(expired);
  CHECK(expired.error() ==
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "private_admission.expired_token"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// An older active admission may finish after a later producer has already opened the account's sole
// fence slot; its first reason occurrence must be inserted ahead of that later loss.
TEST_CASE("active private retention orders an earlier reason before a pending producer loss",
          "[runtime][m4][private-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_private_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  TestPrivateAdmissionOwner owner;
  owner.select_private_turn_completion(TestPrivateAdmissionOwner::Mode::RetainAccount,
                                       oms::PrivateEventDisposition::Applied, std::nullopt);
  owner.select_account_retention_reason(risk::AccountSafetyReason::TimeoutObserved);
  model::DeterministicClockProvider clock{95U};
  runtime::SerializedExecutor executor{
      0U, clock, create_private_admission_configuration_or_throw(authority), owner};
  runtime::DeterministicExecutorDriver driver{executor};
  const auto active_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 69U);
  const auto later_loss_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 70U);
  const auto admitted = executor.try_admit_private(active_attempt);
  REQUIRE(admitted);
  const auto later_loss = executor.try_admit_private(later_loss_attempt);
  REQUIRE(later_loss);
  CHECK(later_loss.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(later_loss.value().attempt_ordinal.value() == 2U);
  CHECK(later_loss.value().account_fence_recorded);

  // ++++++++++++++++++++++++++++++++++++++++
  // Global ordinal selection executes admission one before the already pending fence at ordinal
  // two.
  REQUIRE(driver.bind_to_current_thread());
  const auto retained_turn = driver.execute_next_turn();
  REQUIRE(retained_turn);
  REQUIRE(retained_turn.value().has_value());
  CHECK(retained_turn.value()->kind == runtime::TurnKind::PrivateCommand);
  CHECK(retained_turn.value()->attempt_ordinal == admitted.value().attempt_ordinal);

  // ++++++++++++++++++++++++++++++++++++++++
  // The late callback inserts TimeoutObserved at ordinal one ahead of the existing reason 16 row.
  const auto pending =
      executor.find_account_safety_fence_snapshot(fixture.account_id(), fixture.venue_id());
  REQUIRE(pending.has_value());
  CHECK(pending->pending_lost_attempt_count == 2U);
  CHECK(pending->earliest_pending_attempt_ordinal == admitted.value().attempt_ordinal);
  REQUIRE(pending->reason_occurrence_count == 2U);
  REQUIRE(pending->ordered_unique_reason_occurrences[0U].has_value());
  REQUIRE(pending->ordered_unique_reason_occurrences[1U].has_value());
  CHECK(pending->ordered_unique_reason_occurrences[0U]->reason ==
        risk::AccountSafetyReason::TimeoutObserved);
  CHECK(pending->ordered_unique_reason_occurrences[0U]->first_attempt_ordinal.value() == 1U);
  CHECK(pending->ordered_unique_reason_occurrences[1U]->reason ==
        risk::AccountSafetyReason::CriticalAdmissionLoss);
  CHECK(pending->ordered_unique_reason_occurrences[1U]->first_attempt_ordinal.value() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Owner delivery retains each complete first source fact in the same canonical reason order.
  const auto fence_turn = driver.execute_next_turn();
  REQUIRE(fence_turn);
  REQUIRE(fence_turn.value().has_value());
  CHECK(fence_turn.value()->kind == runtime::TurnKind::AccountSafetyFence);
  REQUIRE(owner.latest_account_fence().has_value());
  REQUIRE(owner.latest_account_fence()->reason_occurrence_count == 2U);
  CHECK(owner.latest_account_fence()->ordered_unique_reason_occurrences[0U]->first_attempt ==
        active_attempt);
  CHECK(owner.latest_account_fence()->ordered_unique_reason_occurrences[1U]->first_attempt ==
        later_loss_attempt);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Private capacity counts pending work only, so extraction frees a slot during owner execution. A
// later account loss then folds its different reason and first provenance into the same fixed
// fence.
TEST_CASE("active private turns free pending capacity and account fences fold all reasons",
          "[runtime][m4][private-admission]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_private_admissions = 1U;
  const auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  test_support::M4PrivateEventFixture fixture;
  TestPrivateAdmissionOwner owner;
  owner.select_private_turn_completion(TestPrivateAdmissionOwner::Mode::RetainAccount,
                                       oms::PrivateEventDisposition::Applied, std::nullopt);
  owner.select_account_retention_reason(risk::AccountSafetyReason::TimeoutObserved);
  model::DeterministicClockProvider clock{100U};
  runtime::SerializedExecutor executor{
      0U, clock, create_private_admission_configuration_or_throw(authority), owner};
  runtime::DeterministicExecutorDriver driver{executor};
  const auto retained_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 71U);
  const auto active_turn_admission =
      create_private_timeout_attempt_or_throw(fixture, authority, 72U);
  const auto later_loss_attempt = create_private_timeout_attempt_or_throw(fixture, authority, 73U);
  const auto admitted = executor.try_admit_private(retained_attempt);
  REQUIRE(admitted);
  CHECK(admitted.value().attempt_ordinal.value() == 1U);
  REQUIRE(driver.bind_to_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
  // Query both evidence oracles from another thread while the owner publishes retained evidence;
  // ThreadSanitizer can therefore qualify the mutex boundary rather than only sequential behavior.
  std::atomic_bool stop_observer{false};
  std::atomic_bool observer_started{false};
  std::atomic_size_t concurrent_observations{0U};
  std::thread observer{[&] {
    observer_started.store(true);
    while (!stop_observer.load()) {
      static_cast<void>(executor.private_admission_observation(admitted.value().attempt_ordinal));
      static_cast<void>(
          owner.find_committed_private_event_disposition(admitted.value().attempt_ordinal));
      static_cast<void>(
          owner.find_committed_retained_private_event_error(admitted.value().attempt_ordinal));
      ++concurrent_observations;
      std::this_thread::yield();
    }
  }};
  while (!observer_started.load()) {
    std::this_thread::yield();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Extraction decrements pending depth before invoking the owner. The callback admission therefore
  // occupies the freed ring slot even though the earlier token remains owner-in-flight.
  owner.admit_during_next_private_turn(executor, active_turn_admission);
  const auto retained_turn = driver.execute_next_turn();
  stop_observer.store(true);
  observer.join();
  REQUIRE(retained_turn);
  REQUIRE(retained_turn.value().has_value());
  CHECK(retained_turn.value()->kind == runtime::TurnKind::PrivateCommand);
  CHECK(concurrent_observations.load() > 0U);
  REQUIRE(owner.private_race_decision().has_value());
  CHECK(owner.private_race_decision()->outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(owner.private_race_decision()->attempt_ordinal.value() == 2U);
  CHECK(owner.private_race_decision()->pending_depth == 1U);
  CHECK_FALSE(owner.private_race_decision()->account_fence_recorded);
  CHECK_FALSE(owner.private_race_error().has_value());
  CHECK(executor.private_lane_snapshot().occupied_slots == 1U);
  CHECK(executor.private_lane_snapshot().queued_slots == 1U);
  CHECK(executor.private_lane_snapshot().in_flight_slots == 0U);
  CHECK(executor.private_lane_snapshot().pending_account_fences == 1U);
  const auto retained = executor.private_admission_observation(admitted.value().attempt_ordinal);
  REQUIRE(retained.has_value());
  CHECK(retained->state == runtime::CriticalPrivateAdmissionState::RetainedForReconciliation);

  // ++++++++++++++++++++++++++++++++++++++++
  // The retained TimeoutObserved fact owns the account's single interval. A later producer loss
  // adds CriticalAdmissionLoss and its first provenance without allocating another fence slot.
  const auto later_loss = executor.try_admit_private(later_loss_attempt);
  REQUIRE(later_loss);
  CHECK(later_loss.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(later_loss.value().attempt_ordinal.value() == 3U);
  CHECK(later_loss.value().account_fence_recorded);
  CHECK(executor.private_lane_snapshot().pending_account_fences == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The canonical fence precedes the queued attempt by shared attempt ordinal and carries both
  // unique causes in first-occurrence order, allowing the owner to escalate through Quarantined.
  const auto fence_turn = driver.execute_next_turn();
  REQUIRE(fence_turn);
  REQUIRE(fence_turn.value().has_value());
  CHECK(fence_turn.value()->kind == runtime::TurnKind::AccountSafetyFence);
  CHECK(fence_turn.value()->attempt_ordinal.value() == 1U);
  REQUIRE(owner.account_fence_count() == 1U);
  CHECK(owner.account_fence_at(0U).lost_attempt_count == 2U);
  REQUIRE(owner.account_fence_at(0U).reason_occurrence_count == 2U);
  REQUIRE(owner.account_fence_at(0U).ordered_unique_reason_occurrences[0U].has_value());
  REQUIRE(owner.account_fence_at(0U).ordered_unique_reason_occurrences[1U].has_value());
  const auto& timeout_occurrence =
      owner.account_fence_at(0U).ordered_unique_reason_occurrences[0U].value();
  CHECK(timeout_occurrence.reason == risk::AccountSafetyReason::TimeoutObserved);
  CHECK(timeout_occurrence.first_attempt == retained_attempt);
  CHECK(timeout_occurrence.first_attempt_ordinal.value() == 1U);
  const auto& quarantine_occurrence =
      owner.account_fence_at(0U).ordered_unique_reason_occurrences[1U].value();
  CHECK(quarantine_occurrence.reason == risk::AccountSafetyReason::CriticalAdmissionLoss);
  CHECK(quarantine_occurrence.first_attempt == later_loss_attempt);
  CHECK(quarantine_occurrence.first_attempt_ordinal.value() == 3U);
  CHECK(executor.private_lane_snapshot().pending_account_fences == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Once the fence succeeds, the callback-admitted value runs normally from the ring slot that was
  // freed before the earlier owner call.
  owner.select_private_turn_completion(TestPrivateAdmissionOwner::Mode::Consume,
                                       oms::PrivateEventDisposition::Applied,
                                       oms::PrivateEventDisposition::Applied);
  const auto admitted_during_active_turn = driver.execute_next_turn();
  REQUIRE(admitted_during_active_turn);
  REQUIRE(admitted_during_active_turn.value().has_value());
  CHECK(admitted_during_active_turn.value()->kind == runtime::TurnKind::PrivateCommand);
  CHECK(admitted_during_active_turn.value()->attempt_ordinal.value() == 2U);
  const auto consumed =
      executor.private_admission_observation(owner.private_race_decision()->attempt_ordinal);
  REQUIRE(consumed.has_value());
  CHECK(consumed->state == runtime::CriticalPrivateAdmissionState::EconomicallyConsumed);
  CHECK(executor.private_lane_snapshot().occupied_slots == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
