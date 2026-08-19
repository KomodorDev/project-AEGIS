// Purpose: prove ordinary attempt decisions, bounded command/fence ordering, terminal clock and
// counter policy, owner non-reentry, and identical deterministic/dedicated executor turns.

#include "aegis/runtime/dedicated_executor_driver.hpp"
#include "aegis/runtime/serialized_executor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <limits>
#include <optional>
#include <thread>
#include <type_traits>

namespace {

using namespace aegis;

// ########################################################################
// FixedRecorder keeps handler effects in already-owned bounded storage.
struct FixedRecorder {
  std::array<int, 32U> values{};
  std::array<std::optional<runtime::AcceptedTurnContext>, 32U> contexts{};
  std::size_t size{0U};

  // --------------------------------------------------------
  // Append one value without allocating or exceeding the test's authored capacity.
  void append(int value) noexcept {
    values[size] = value;
    ++size;
  }

  // --------------------------------------------------------
  // Retain a copy of stack-scoped command authority only for exact context assertions.
  void append(int value, const runtime::AcceptedTurnContext& context) noexcept {
    values[size] = value;
    contexts[size].emplace(context);
    ++size;
  }

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// RecordCommand copies one stable recorder pointer and value into WorkItem inline storage.
struct RecordCommand {
  FixedRecorder* recorder;
  int value;
};

// ########################################################################

// ########################################################################
// FenceRecorder applies source-control turns to the same ordered log while retaining exact fence
// payloads for merge assertions.
class FenceRecorder final : public runtime::SourceDiscontinuityHandler {
public:

  // --------------------------------------------------------
  // Borrow bounded state whose lifetime encloses the tested executor.
  explicit FenceRecorder(FixedRecorder& order) noexcept : order_{order} {}

  // --------------------------------------------------------
  // Record fences as negative source ordinals and preserve their complete immutable summaries.
  [[nodiscard]] model::Result<void>
  on_source_discontinuity(const runtime::SourceDiscontinuity& discontinuity,
                          const runtime::ControlTurnContext&) noexcept override {
    order_.append(-static_cast<int>(discontinuity.source_ordinal.value()));
    fences_[size_].emplace(discontinuity);
    ++size_;
    if (completion_latch_ != nullptr) {
      completion_latch_->count_down();
    }
    return model::Result<void>::success();
  }

  // --------------------------------------------------------
  // Attach optional preallocated synchronization for the dedicated-owner wake test.
  void set_completion_latch(std::latch& completion_latch) noexcept {
    completion_latch_ = &completion_latch;
  }

  // --------------------------------------------------------
  // Borrow one recorded fence by stable insertion position.
  [[nodiscard]] const runtime::SourceDiscontinuity& at(std::size_t index) const noexcept {
    return fences_[index].value();
  }

  // --------------------------------------------------------
  // Return the exact number of handled owner-control turns.
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  // --------------------------------------------------------
private:
  FixedRecorder& order_;
  std::array<std::optional<runtime::SourceDiscontinuity>, 8U> fences_{};
  std::size_t size_{0U};
  std::latch* completion_latch_{nullptr};
};

// ########################################################################

// ########################################################################
// FailingFenceHandler admits one same-source successor while the extracted fence is in flight,
// then returns a pre-mutation validation failure so restoration and merge can be observed.
class FailingFenceHandler final : public runtime::SourceDiscontinuityHandler {
public:

  // --------------------------------------------------------
  // Install stable runtime handles after the executor has borrowed this handler.
  void set_runtime(runtime::SerializedExecutor& executor,
                   const runtime::WorkItem& successor_work) noexcept {
    executor_ = &executor;
    successor_work_ = &successor_work;
  }

  // --------------------------------------------------------
  // Extend the in-flight source interval, then fail before changing any data-plane state.
  [[nodiscard]] model::Result<void>
  on_source_discontinuity(const runtime::SourceDiscontinuity& discontinuity,
                          const runtime::ControlTurnContext& context) noexcept override {
    context_.emplace(context);
    const auto admission = executor_->try_admit(*successor_work_, discontinuity.source_ordinal);
    if (admission) {
      successor_decision_.emplace(admission.value());
    }
    return model::Result<void>::failure(model::DomainError::at_field(
        model::DomainErrorCode::InvalidMarketState, "test_fence_handler"));
  }

  // --------------------------------------------------------
  // Borrow the nested source-gating decision made while the fence was in flight.
  [[nodiscard]] const std::optional<runtime::AdmissionDecision>&
  successor_decision() const noexcept {
    return successor_decision_;
  }

  // --------------------------------------------------------
  // Borrow the exact stack-scoped control values copied by the test handler.
  [[nodiscard]] const std::optional<runtime::ControlTurnContext>& context() const noexcept {
    return context_;
  }

  // --------------------------------------------------------
private:
  runtime::SerializedExecutor* executor_{nullptr};
  const runtime::WorkItem* successor_work_{nullptr};
  std::optional<runtime::AdmissionDecision> successor_decision_;
  std::optional<runtime::ControlTurnContext> context_;
};

// ########################################################################

// ########################################################################
// ReentryObservation retains stable outcomes produced from inside one active owner turn.
struct ReentryObservation {
  runtime::SerializedExecutor* executor;
  const runtime::WorkItem* follow_up;
  bool nested_run_rejected{false};
  bool nested_drive_rejected{false};
  model::DomainErrorCode nested_run_error{model::DomainErrorCode::InvalidValue};
  model::DomainErrorCode nested_drive_error{model::DomainErrorCode::InvalidValue};
  std::optional<runtime::AdmissionDecision> follow_up_decision;
};

// ########################################################################

// ########################################################################
// ReentryCommand keeps the mutable observation outside its immutable copied work value.
struct ReentryCommand {
  ReentryObservation* observation;
};

// ########################################################################

// ########################################################################
// TerminalOverlapObservation captures a successful active handler whose nested producer attempt
// publishes a terminal executor fault before that owner turn returns.
struct TerminalOverlapObservation {
  runtime::SerializedExecutor* executor;
  const runtime::WorkItem* follow_up;
  std::size_t applied{0U};
  std::optional<runtime::AcceptedTurnContext> context;
  std::optional<model::DomainError> nested_error;
};

// ########################################################################

// ########################################################################
// TerminalOverlapCommand keeps mutable lifecycle evidence behind one stable runtime handle.
struct TerminalOverlapCommand {
  TerminalOverlapObservation* observation;
};

// ########################################################################

// ########################################################################
// CompletionCommand synchronizes a dedicated owner turn without sharing non-atomic mutable data.
struct CompletionCommand {
  std::atomic_size_t* completed;
  std::latch* completion_latch;
};

// ########################################################################

// ########################################################################
// StopGateCommand holds one active dedicated turn until its test has synchronized stop or close.
struct StopGateCommand {
  std::atomic_size_t* completed;
  std::latch* started;
  std::latch* release;
};

// ########################################################################

// ########################################################################
// ScriptedClockProvider returns an exact bounded observation sequence and then repeats its final
// value, allowing deterministic admission or processing regression.
class ScriptedClockProvider final : public model::ClockProvider {
public:

  // --------------------------------------------------------
  // Copy the complete short script before the executor begins reading it.
  explicit ScriptedClockProvider(std::array<std::uint64_t, 4U> values) noexcept : values_{values} {}

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Consume one raw monotonic value through the ClockProvider nominal wrappers.
  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override {
    if (position_ < values_.size()) {
      return values_[position_++];
    }
    return values_.back();
  }

  // --------------------------------------------------------
  std::array<std::uint64_t, 4U> values_;
  std::size_t position_{0U};
};

// ########################################################################

// --------------------------------------------------------
// Append one copied command value to bounded owner-local state.
[[nodiscard]] model::Result<void>
record_value(const RecordCommand& command, const runtime::AcceptedTurnContext& context) noexcept {
  command.recorder->append(command.value, context);
  return model::Result<void>::success();
}

// --------------------------------------------------------
// Prove nested run and drive fail before admitting follow-up work for a later turn.
[[nodiscard]] model::Result<void> observe_reentry(const ReentryCommand& command,
                                                  const runtime::AcceptedTurnContext&) noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Both progression APIs must reject the active owner call stack with the same stable error.
  const auto nested_run = command.observation->executor->run_one();
  if (!nested_run) {
    command.observation->nested_run_rejected = true;
    command.observation->nested_run_error = nested_run.error().code;
  }
  const auto nested_drive = command.observation->executor->drive(1U);
  if (!nested_drive) {
    command.observation->nested_drive_rejected = true;
    command.observation->nested_drive_error = nested_drive.error().code;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Admission may use the slot released at turn start, but cannot execute recursively.
  const auto admission = command.observation->executor->try_admit(*command.observation->follow_up);
  if (admission) {
    command.observation->follow_up_decision.emplace(admission.value());
  }
  return model::Result<void>::success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Apply one owner mutation, then trigger producer-side attempt exhaustion while the turn is active.
[[nodiscard]] model::Result<void>
apply_then_exhaust_admission(const TerminalOverlapCommand& command,
                             const runtime::AcceptedTurnContext& context) noexcept {
  ++command.observation->applied;
  command.observation->context.emplace(context);
  const auto nested = command.observation->executor->try_admit(*command.observation->follow_up);
  if (!nested) {
    command.observation->nested_error.emplace(nested.error());
  }
  return model::Result<void>::success();
}

// --------------------------------------------------------
// Publish one dedicated command completion using storage prepared before admission.
[[nodiscard]] model::Result<void> count_completion(const CompletionCommand& command,
                                                   const runtime::AcceptedTurnContext&) noexcept {
  command.completed->fetch_add(1U);
  command.completion_latch->count_down();
  return model::Result<void>::success();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Hold one owner turn open so stop publication can deterministically win before the next turn.
[[nodiscard]] model::Result<void> wait_for_stop_gate(const StopGateCommand& command,
                                                     const runtime::AcceptedTurnContext&) noexcept {
  const auto prior_completed = command.completed->fetch_add(1U);
  if (prior_completed == 0U) {
    command.started->count_down();
    command.release->wait();
  }
  return model::Result<void>::success();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Return a validation failure without mutating the recorder named by the copied command.
[[nodiscard]] model::Result<void> reject_command(const RecordCommand&,
                                                 const runtime::AcceptedTurnContext&) noexcept {
  return model::Result<void>::failure(model::DomainError::at_field(
      model::DomainErrorCode::InvalidMarketState, "test_command_handler"));
}

// --------------------------------------------------------

// ########################################################################
// Compile-time constraints pin the allocation-free copied-command boundary.
constexpr std::size_t raw_drive_limit = std::numeric_limits<std::size_t>::max();
static_assert(static_cast<std::uint8_t>(runtime::AdmissionOutcome::Accepted) == 1U);
static_assert(static_cast<std::uint8_t>(runtime::AdmissionOutcome::CapacityExceeded) == 2U);
static_assert(static_cast<std::uint8_t>(runtime::AdmissionOutcome::Closed) == 3U);
static_assert(static_cast<std::uint8_t>(runtime::TurnKind::Command) == 1U);
static_assert(static_cast<std::uint8_t>(runtime::TurnKind::SourceDiscontinuity) == 2U);
static_assert(std::is_trivially_copyable_v<RecordCommand>);
static_assert(std::is_trivially_copyable_v<ReentryCommand>);
static_assert(std::is_trivially_copyable_v<TerminalOverlapCommand>);
static_assert(std::is_trivially_copyable_v<CompletionCommand>);
static_assert(std::is_trivially_copyable_v<StopGateCommand>);
static_assert(std::is_trivially_copyable_v<runtime::WorkItem>);
static_assert(!std::is_constructible_v<runtime::WorkItem, std::function<void()>>);

// ########################################################################

// --------------------------------------------------------
// Accepted work receives separate attempt and receive sequences, while a full unattributable
// attempt remains an ordinary decision and perturbs neither receive sequence nor the clock.
TEST_CASE("admission decisions pin attempts receipts queue age and ring reuse",
          "[runtime][executor]") {
  model::DeterministicClockProvider clock{10U};
  runtime::SerializedExecutor executor{2U, clock};
  runtime::DeterministicExecutorDriver driver{executor};
  FixedRecorder recorder;

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy two values at authored times, then observe overload as attempt three.
  auto original = RecordCommand{&recorder, 1};
  const auto first_work = runtime::WorkItem::make<&record_value>(original);
  original.value = 99;
  const auto first = executor.try_admit(first_work);
  REQUIRE(first);
  CHECK(first.value() ==
        runtime::AdmissionDecision{runtime::AdmissionOutcome::Accepted,
                                   model::AdmissionOrdinal::initial(), 1U, 2U,
                                   runtime::AdmissionReceipt{model::AdmissionOrdinal::initial(),
                                                             model::ReceiveSequence::initial(),
                                                             model::ReceiveTimestamp{10U}, 1U, 2U},
                                   false});

  REQUIRE(clock.advance(2U));
  const auto second =
      executor.try_admit(runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 2}));
  REQUIRE(second);
  REQUIRE(second.value().receipt.has_value());
  CHECK(second.value().attempt_ordinal.value() == 2U);
  CHECK(second.value().receipt->receive_sequence.value() == 2U);
  CHECK(second.value().receipt->received_at == model::ReceiveTimestamp{12U});
  CHECK(second.value().receipt->pending_depth == 2U);

  const auto full =
      executor.try_admit(runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 3}));
  REQUIRE(full);
  CHECK(full.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(full.value().attempt_ordinal.value() == 3U);
  CHECK(full.value().pending_depth == 2U);
  CHECK(full.value().pending_capacity == 2U);
  CHECK_FALSE(full.value().receipt.has_value());
  CHECK_FALSE(full.value().discontinuity_recorded);

  // ++++++++++++++++++++++++++++++++++++++++
  // One owner turn reports exact queue age, then ring reuse assigns attempt four but receive three.
  REQUIRE(driver.bind_to_current_thread());
  REQUIRE(clock.advance(3U));
  const auto first_turn = driver.run_one();
  REQUIRE(first_turn);
  REQUIRE(first_turn.value().has_value());
  CHECK(first_turn.value()->kind == runtime::TurnKind::Command);
  CHECK(first_turn.value()->attempt_ordinal.value() == 1U);
  REQUIRE(first_turn.value()->queue_age.has_value());
  CHECK(first_turn.value()->queue_age->value() == 5U);
  CHECK(first_turn.value()->pending_commands_at_start == 1U);
  CHECK(first_turn.value()->pending_commands_at_completion == 1U);
  CHECK(recorder.values[0] == 1);
  REQUIRE(recorder.contexts[0].has_value());
  CHECK(recorder.contexts[0].value() ==
        runtime::AcceptedTurnContext{first.value().receipt.value(), model::TurnOrdinal::initial(),
                                     model::ProcessingTimestamp{15U},
                                     model::ElapsedNanoseconds{5U}});

  const auto third =
      executor.try_admit(runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 3}));
  REQUIRE(third);
  CHECK(third.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(third.value().attempt_ordinal.value() == 4U);
  REQUIRE(third.value().receipt.has_value());
  CHECK(third.value().receipt->receive_sequence.value() == 3U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Bounded drive drains only accepted commands and retains lifetime completion state.
  const auto drained = driver.drive(8U);
  REQUIRE(drained);
  CHECK(drained.value() == runtime::DriveReport{2U, 0U, 0U, 2U, raw_drive_limit, 3U});
  REQUIRE(recorder.size == 3U);
  CHECK(recorder.values[1] == 2);
  CHECK(recorder.values[2] == 3);
  CHECK(executor.snapshot() ==
        runtime::QueueSnapshot{0U, 0U, 0U, 2U, 0U, raw_drive_limit, 3U, false, false, true, false});
  REQUIRE(driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// CapacityExceeded and Closed are successful ordinary values, and closure attempts continue the
// attempt sequence without reading clocks, consuming queue slots, or creating fences.
TEST_CASE("capacity and closure remain ordinary replayable decisions", "[runtime][executor]") {
  model::DeterministicClockProvider clock;
  runtime::SerializedExecutor executor{0U, clock};
  FixedRecorder recorder;
  const auto work = runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 1});

  // ++++++++++++++++++++++++++++++++++++++++
  // An open zero-slot executor reports attempt one as unattributable capacity pressure.
  const auto full = executor.try_admit(work);
  REQUIRE(full);
  CHECK(full.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(full.value().attempt_ordinal.value() == 1U);
  CHECK(full.value().pending_depth == 0U);
  CHECK(full.value().pending_capacity == 0U);
  CHECK_FALSE(full.value().discontinuity_recorded);

  // ++++++++++++++++++++++++++++++++++++++++
  // A typed but unconfigured source observes the same capacity without inventing a source fence.
  const auto unconfigured = executor.try_admit(work, model::MarketSourceOrdinal::initial());
  REQUIRE(unconfigured);
  CHECK(unconfigured.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(unconfigured.value().attempt_ordinal.value() == 2U);
  CHECK(unconfigured.value().pending_depth == 0U);
  CHECK(unconfigured.value().pending_capacity == 0U);
  CHECK_FALSE(unconfigured.value().discontinuity_recorded);

  // ++++++++++++++++++++++++++++++++++++++++
  // Idempotent closure reports attempts three and four as Closed without inventing receipts.
  executor.close();
  executor.close();
  const auto closed_two = executor.try_admit(work);
  const auto closed_three = executor.try_admit(work);
  REQUIRE(closed_two);
  REQUIRE(closed_three);
  CHECK(closed_two.value().outcome == runtime::AdmissionOutcome::Closed);
  CHECK(closed_two.value().attempt_ordinal.value() == 3U);
  CHECK(closed_two.value().pending_depth == 0U);
  CHECK(closed_two.value().pending_capacity == 0U);
  CHECK_FALSE(closed_two.value().receipt.has_value());
  CHECK_FALSE(closed_two.value().discontinuity_recorded);
  CHECK(closed_three.value().attempt_ordinal.value() == 4U);
  CHECK(executor.snapshot() ==
        runtime::QueueSnapshot{0U, 0U, 0U, 0U, 0U, raw_drive_limit, 0U, true, false, false, false});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Simultaneous producers must linearize through one attempt sequence and expose the exact queue
// boundary without races, duplicate ordinals, or hidden rejected work.
TEST_CASE("concurrent producers preserve exact admission capacity and ordinals",
          "[runtime][executor][concurrency]") {
  constexpr std::size_t producer_count = 16U;
  constexpr std::size_t queue_capacity = 8U;
  model::SystemClockProvider clock;
  runtime::SerializedExecutor executor{queue_capacity, clock};
  FixedRecorder recorder;
  const auto work = runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 1});
  std::array<std::optional<runtime::AdmissionDecision>, producer_count> decisions{};
  std::array<std::thread, producer_count> producers{};
  std::latch ready{producer_count};
  std::latch start{1U};

  // ++++++++++++++++++++++++++++++++++++++++
  // Release all producers from one gate while retaining each result in a disjoint array slot.
  for (std::size_t index = 0U; index < producer_count; ++index) {
    producers[index] = std::thread{[&executor, &work, &decisions, &ready, &start, index] {
      ready.count_down();
      start.wait();
      const auto decision = executor.try_admit(work);
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
  // Mutex order may choose producers arbitrarily, but the resulting first eight attempts are the
  // accepted prefix and every later decision sees the exact full depth.
  std::array<bool, producer_count + 1U> seen{};
  for (const auto& decision : decisions) {
    REQUIRE(decision.has_value());
    const auto attempt = static_cast<std::size_t>(decision->attempt_ordinal.value());
    REQUIRE(attempt > 0U);
    REQUIRE(attempt <= producer_count);
    CHECK_FALSE(seen[attempt]);
    seen[attempt] = true;
    CHECK(decision->pending_capacity == queue_capacity);
    if (attempt <= queue_capacity) {
      CHECK(decision->outcome == runtime::AdmissionOutcome::Accepted);
      CHECK(decision->pending_depth == attempt);
      REQUIRE(decision->receipt.has_value());
      CHECK(decision->receipt->receive_sequence.value() == attempt);
    } else {
      CHECK(decision->outcome == runtime::AdmissionOutcome::CapacityExceeded);
      CHECK(decision->pending_depth == queue_capacity);
      CHECK_FALSE(decision->receipt.has_value());
    }
  }
  CHECK(std::all_of(seen.begin() + 1, seen.end(), [](bool value) { return value; }));
  CHECK(executor.snapshot().pending_commands == queue_capacity);

  // ++++++++++++++++++++++++++++++++++++++++
  // The single owner subsequently drains exactly the accepted prefix.
  runtime::DeterministicExecutorDriver driver{executor};
  REQUIRE(driver.bind_to_current_thread());
  const auto drained = driver.drive(queue_capacity);
  REQUIRE(drained);
  CHECK(drained.value().turns_executed == queue_capacity);
  CHECK(recorder.size == queue_capacity);
  REQUIRE(driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Concurrent attributable rejections must fold into one preallocated source fence, and admission
// racing closure must have one observable linearization order.
TEST_CASE("concurrent source loss and closure remain linearizable",
          "[runtime][executor][concurrency]") {
  constexpr std::size_t producer_count = 16U;
  model::SystemClockProvider fence_clock;
  FixedRecorder order;
  FenceRecorder fence_handler{order};
  runtime::SerializedExecutor fenced_executor{0U, 1U, fence_clock, fence_handler};
  const auto source = model::MarketSourceOrdinal::initial();
  const auto fence_work = runtime::WorkItem::make<&record_value>(RecordCommand{&order, 1});
  std::array<std::optional<runtime::AdmissionDecision>, producer_count> decisions{};
  std::array<std::thread, producer_count> producers{};
  std::latch ready{producer_count};
  std::latch start{1U};

  // ++++++++++++++++++++++++++++++++++++++++
  // All zero-capacity attempts target one configured source from simultaneous producer threads.
  for (std::size_t index = 0U; index < producer_count; ++index) {
    producers[index] =
        std::thread{[&fenced_executor, &fence_work, &decisions, &ready, &start, source, index] {
          ready.count_down();
          start.wait();
          const auto decision = fenced_executor.try_admit(fence_work, source);
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
  // Every attempt is explicit while the one owner-visible fence retains the earliest ordinal and
  // exact folded occurrence count.
  std::array<bool, producer_count + 1U> seen{};
  for (const auto& decision : decisions) {
    REQUIRE(decision.has_value());
    CHECK(decision->outcome == runtime::AdmissionOutcome::CapacityExceeded);
    CHECK(decision->discontinuity_recorded);
    const auto attempt = static_cast<std::size_t>(decision->attempt_ordinal.value());
    REQUIRE(attempt > 0U);
    REQUIRE(attempt <= producer_count);
    CHECK_FALSE(seen[attempt]);
    seen[attempt] = true;
  }
  REQUIRE(fenced_executor.source_fence_snapshot(source).has_value());
  CHECK(fenced_executor.source_fence_snapshot(source).value() ==
        runtime::SourceFenceSnapshot{source, model::AdmissionOrdinal::initial(), producer_count,
                                     false});
  runtime::DeterministicExecutorDriver fence_driver{fenced_executor};
  REQUIRE(fence_driver.bind_to_current_thread());
  const auto fence_turn = fence_driver.run_one();
  REQUIRE(fence_turn);
  REQUIRE(fence_turn.value().has_value());
  REQUIRE(fence_turn.value()->discontinuity.has_value());
  CHECK(fence_turn.value()->discontinuity->lost_attempt_count == producer_count);
  REQUIRE(fence_driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
  // Repeated two-thread races accept the lone command or close before it, then a later attempt
  // always observes Closed at ordinal two with state matching that first outcome.
  for (std::size_t iteration = 0U; iteration < 32U; ++iteration) {
    model::SystemClockProvider close_clock;
    runtime::SerializedExecutor executor{1U, close_clock};
    FixedRecorder recorder;
    const auto work = runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 1});
    std::optional<runtime::AdmissionDecision> first;
    std::latch race_start{1U};
    std::thread producer{[&executor, &work, &first, &race_start] {
      race_start.wait();
      const auto decision = executor.try_admit(work);
      if (decision) {
        first.emplace(decision.value());
      }
    }};
    std::thread closer{[&executor, &race_start] {
      race_start.wait();
      executor.close();
    }};
    race_start.count_down();
    producer.join();
    closer.join();

    REQUIRE(first.has_value());
    CHECK(first->attempt_ordinal == model::AdmissionOrdinal::initial());
    CHECK((first->outcome == runtime::AdmissionOutcome::Accepted ||
           first->outcome == runtime::AdmissionOutcome::Closed));
    const auto later = executor.try_admit(work);
    REQUIRE(later);
    CHECK(later.value().outcome == runtime::AdmissionOutcome::Closed);
    CHECK(later.value().attempt_ordinal.value() == 2U);
    CHECK(executor.snapshot().closed);
    CHECK(executor.snapshot().pending_commands ==
          (first->outcome == runtime::AdmissionOutcome::Accepted ? 1U : 0U));
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A source remains gated from its first capacity loss through fence completion, so no apparent
// recovery command can cross the invalidating owner-control turn.
TEST_CASE("source loss intervals gate recovery and preserve global attempt order",
          "[runtime][executor]") {
  model::DeterministicClockProvider clock;
  FixedRecorder order;
  FenceRecorder fences{order};
  runtime::SerializedExecutor executor{2U, 2U, clock, fences};
  runtime::DeterministicExecutorDriver driver{executor};
  const auto source_one = model::MarketSourceOrdinal::initial();
  const auto source_two = model::MarketSourceOrdinal::from_value(2U).value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Accept attempts one and two, then record source one's capacity loss at attempt three.
  REQUIRE(executor.try_admit(runtime::WorkItem::make<&record_value>(RecordCommand{&order, 10}),
                             source_one));
  REQUIRE(executor.try_admit(runtime::WorkItem::make<&record_value>(RecordCommand{&order, 20}),
                             source_two));
  const auto lost_three = executor.try_admit(
      runtime::WorkItem::make<&record_value>(RecordCommand{&order, 30}), source_one);
  REQUIRE(lost_three);
  CHECK(lost_three.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(lost_three.value().attempt_ordinal.value() == 3U);
  CHECK(lost_three.value().discontinuity_recorded);

  // ++++++++++++++++++++++++++++++++++++++++
  // After command one opens a slot, source-one attempts four and five remain CapacityExceeded and
  // extend the same fence even though FIFO capacity exists. Source two may use that slot at six.
  REQUIRE(driver.bind_to_current_thread());
  REQUIRE(driver.run_one());
  const auto gated_four = executor.try_admit(
      runtime::WorkItem::make<&record_value>(RecordCommand{&order, 40}), source_one);
  const auto gated_five = executor.try_admit(
      runtime::WorkItem::make<&record_value>(RecordCommand{&order, 50}), source_one);
  const auto accepted_six = executor.try_admit(
      runtime::WorkItem::make<&record_value>(RecordCommand{&order, 60}), source_two);
  REQUIRE(gated_four);
  REQUIRE(gated_five);
  REQUIRE(accepted_six);
  CHECK(gated_four.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(gated_four.value().attempt_ordinal.value() == 4U);
  CHECK(gated_four.value().pending_depth == 1U);
  CHECK(gated_four.value().pending_capacity == 2U);
  CHECK(gated_four.value().discontinuity_recorded);
  CHECK(gated_five.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(gated_five.value().attempt_ordinal.value() == 5U);
  CHECK(gated_five.value().discontinuity_recorded);
  CHECK(accepted_six.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(accepted_six.value().attempt_ordinal.value() == 6U);
  REQUIRE(executor.source_fence_snapshot(source_one).has_value());
  CHECK(executor.source_fence_snapshot(source_one).value() ==
        runtime::SourceFenceSnapshot{source_one, model::AdmissionOrdinal::from_value(3U).value(),
                                     3U, false});

  // ++++++++++++++++++++++++++++++++++++++++
  // Command two and the attempt-three fence run before accepted attempt six; all three losses fold
  // into the one exact interval rooted at attempt three.
  const auto through_fence = driver.drive(2U);
  REQUIRE(through_fence);
  CHECK(through_fence.value().turns_executed == 2U);
  REQUIRE(fences.size() == 1U);
  CHECK(fences.at(0) == runtime::SourceDiscontinuity{
                            source_one, model::AdmissionOrdinal::from_value(3U).value(), 3U});
  REQUIRE(order.size == 3U);
  CHECK(order.values[0] == 10);
  CHECK(order.values[1] == 20);
  CHECK(order.values[2] == -1);

  // ++++++++++++++++++++++++++++++++++++++++
  // Only after fence completion may source one admit recovery attempt seven, which remains behind
  // already accepted attempt six in FIFO order.
  const auto recovery_seven = executor.try_admit(
      runtime::WorkItem::make<&record_value>(RecordCommand{&order, 70}), source_one);
  REQUIRE(recovery_seven);
  CHECK(recovery_seven.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(recovery_seven.value().attempt_ordinal.value() == 7U);
  const auto remainder = driver.drive(8U);
  REQUIRE(remainder);
  CHECK(remainder.value().turns_executed == 2U);
  REQUIRE(driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
  // The final order proves neither gated source-one command ran before its loss fence.
  REQUIRE(order.size == 5U);
  CHECK(order.values[3] == 60);
  CHECK(order.values[4] == 70);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Closing freezes later admission as Closed while draining the already recorded command/fence
// prefix, including a fence that occupies no FIFO slot.
TEST_CASE("close drains the established command and fence prefix", "[runtime][executor]") {
  model::DeterministicClockProvider clock;
  FixedRecorder order;
  FenceRecorder fences{order};
  runtime::SerializedExecutor executor{1U, 1U, clock, fences};
  const auto source = model::MarketSourceOrdinal::initial();
  const auto work = runtime::WorkItem::make<&record_value>(RecordCommand{&order, 1});
  REQUIRE(executor.try_admit(work, source));
  REQUIRE(executor.try_admit(work, source));
  executor.close();

  // ++++++++++++++++++++++++++++++++++++++++
  // A later Closed attempt consumes ordinal three but cannot merge into the pending fence.
  const auto closed = executor.try_admit(work, source);
  REQUIRE(closed);
  CHECK(closed.value().outcome == runtime::AdmissionOutcome::Closed);
  CHECK(closed.value().attempt_ordinal.value() == 3U);
  CHECK_FALSE(closed.value().discontinuity_recorded);

  // ++++++++++++++++++++++++++++++++++++++++
  // Manual progression drains attempts one and two and then reports stable closed-empty state.
  runtime::DeterministicExecutorDriver driver{executor};
  REQUIRE(driver.bind_to_current_thread());
  const auto drained = driver.drive(8U);
  REQUIRE(drained);
  CHECK(drained.value() == runtime::DriveReport{2U, 0U, 0U, 1U, raw_drive_limit, 2U});
  CHECK(order.values[0] == 1);
  CHECK(order.values[1] == -1);
  const auto empty = driver.run_one();
  REQUIRE(empty);
  CHECK_FALSE(empty.value().has_value());
  REQUIRE(driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Accepted handler failure consumes its command but certifies pre-mutation rejection, publishes no
// TurnReport, and advances neither turn identity nor completed count.
TEST_CASE("accepted handler failure is terminal without turn publication", "[runtime][executor]") {
  model::DeterministicClockProvider clock;
  runtime::SerializedExecutor executor{1U, clock};
  runtime::DeterministicExecutorDriver driver{executor};
  FixedRecorder recorder;
  REQUIRE(
      executor.try_admit(runtime::WorkItem::make<&reject_command>(RecordCommand{&recorder, 1})));
  REQUIRE(driver.bind_to_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
  // The handler error is the stable terminal cause and its forbidden recorder remains untouched.
  const auto rejected = driver.run_one();
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == model::DomainError::at_field(model::DomainErrorCode::InvalidMarketState,
                                                         "test_command_handler"));
  CHECK(executor.terminal_error() == rejected.error());
  CHECK(recorder.size == 0U);
  CHECK(executor.snapshot() ==
        runtime::QueueSnapshot{0U, 0U, 0U, 1U, 0U, raw_drive_limit, 0U, true, true, true, false});
  REQUIRE(driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A successful active handler remains a completed, reportable turn when its nested producer call
// concurrently publishes terminal admission exhaustion.
TEST_CASE("applied turn reports before overlapping producer fault surfaces",
          "[runtime][executor]") {
  model::DeterministicClockProvider clock;
  const auto penultimate_attempt =
      model::AdmissionOrdinal::from_value(std::numeric_limits<std::uint64_t>::max() - 1U).value();
  runtime::SerializedExecutor executor{
      1U, clock, raw_drive_limit,
      runtime::ExecutorCounterSeed{penultimate_attempt, std::nullopt, std::nullopt}};
  runtime::DeterministicExecutorDriver driver{executor};
  FixedRecorder recorder;
  const auto follow_up = runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 2});
  TerminalOverlapObservation observation{&executor, &follow_up, 0U, std::nullopt, std::nullopt};
  const auto admission = executor.try_admit(
      runtime::WorkItem::make<&apply_then_exhaust_admission>(TerminalOverlapCommand{&observation}));
  REQUIRE(admission);
  REQUIRE(driver.bind_to_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
  // The active command owns the maximum attempt, applies once, and reports completed turn one even
  // though its nested next attempt fails closed before handler return.
  const auto applied = driver.run_one();
  REQUIRE(applied);
  REQUIRE(applied.value().has_value());
  CHECK(applied.value()->attempt_ordinal.value() == std::numeric_limits<std::uint64_t>::max());
  CHECK(applied.value()->turn_ordinal.value() == 1U);
  CHECK(applied.value()->completed_turns == 1U);
  CHECK(observation.applied == 1U);
  REQUIRE(observation.context.has_value());
  CHECK(observation.context->turn_ordinal.value() == 1U);
  REQUIRE(observation.nested_error.has_value());
  CHECK(observation.nested_error->code == model::DomainErrorCode::ExecutorCounterExhausted);
  CHECK(executor.snapshot().completed_turns == 1U);
  CHECK(executor.snapshot().faulted);

  // ++++++++++++++++++++++++++++++++++++++++
  // The stored producer fault surfaces unchanged at each subsequent progression boundary.
  const auto next_run = driver.run_one();
  const auto next_drive = driver.drive(1U);
  REQUIRE_FALSE(next_run);
  REQUIRE_FALSE(next_drive);
  CHECK(next_run.error() == observation.nested_error.value());
  CHECK(next_drive.error() == observation.nested_error.value());
  REQUIRE(driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Failed fence handling restores the extracted interval, folds a concurrently gated successor,
// and retains the handler error without publishing its turn ordinal.
TEST_CASE("fence handler failure restores in-flight and successor losses", "[runtime][executor]") {
  model::DeterministicClockProvider clock;
  FailingFenceHandler fence_handler;
  runtime::SerializedExecutor executor{1U, 1U, clock, fence_handler};
  runtime::DeterministicExecutorDriver driver{executor};
  FixedRecorder recorder;
  const auto source = model::MarketSourceOrdinal::initial();
  const auto work = runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 1});
  fence_handler.set_runtime(executor, work);
  REQUIRE(executor.try_admit(work, source));
  const auto initial_loss = executor.try_admit(work, source);
  REQUIRE(initial_loss);
  CHECK(initial_loss.value().attempt_ordinal.value() == 2U);
  REQUIRE(driver.bind_to_current_thread());
  REQUIRE(driver.run_one());

  // ++++++++++++++++++++++++++++++++++++++++
  // In-flight attempt three is gated despite an empty queue, then the handler failure restores the
  // attempt-two fence ahead of that successor without completing control turn two.
  const auto failed_fence = driver.run_one();
  REQUIRE_FALSE(failed_fence);
  CHECK(failed_fence.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidMarketState,
                                     "test_fence_handler"));
  REQUIRE(fence_handler.successor_decision().has_value());
  CHECK(fence_handler.successor_decision()->outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(fence_handler.successor_decision()->attempt_ordinal.value() == 3U);
  CHECK(fence_handler.successor_decision()->discontinuity_recorded);
  REQUIRE(fence_handler.context().has_value());
  CHECK(fence_handler.context()->turn_ordinal.value() == 2U);
  CHECK(recorder.size == 1U);
  CHECK(executor.snapshot() ==
        runtime::QueueSnapshot{0U, 1U, 0U, 1U, 1U, raw_drive_limit, 1U, true, true, true, false});
  REQUIRE(executor.source_fence_snapshot(source).has_value());
  CHECK(executor.source_fence_snapshot(source).value() ==
        runtime::SourceFenceSnapshot{source, model::AdmissionOrdinal::from_value(2U).value(), 2U,
                                     false});
  REQUIRE(driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Binding rejects implicit execution and cross-thread access while permitting deliberate release
// and successor handoff.
TEST_CASE("only the bound owner can execute and release turns", "[runtime][executor]") {
  model::DeterministicClockProvider clock;
  runtime::SerializedExecutor executor{1U, clock};

  // ++++++++++++++++++++++++++++++++++++++++
  // Both one-turn and zero-turn progression require an explicit owner.
  const auto unbound_run = executor.run_one();
  const auto unbound_drive = executor.drive(0U);
  REQUIRE_FALSE(unbound_run);
  REQUIRE_FALSE(unbound_drive);
  CHECK(unbound_run.error().code == model::DomainErrorCode::ExecutorNotBound);
  CHECK(unbound_drive.error().code == model::DomainErrorCode::ExecutorNotBound);
  REQUIRE(executor.bind_to_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
  // Another thread receives WrongOwner for both progression and release.
  std::optional<model::DomainError> run_error;
  std::optional<model::DomainError> release_error;
  std::thread intruder{[&executor, &run_error, &release_error] {
    const auto run = executor.run_one();
    if (!run) {
      run_error.emplace(run.error());
    }
    const auto release = executor.release_from_current_thread();
    if (!release) {
      release_error.emplace(release.error());
    }
  }};
  intruder.join();
  REQUIRE(run_error.has_value());
  REQUIRE(release_error.has_value());
  CHECK(run_error->code == model::DomainErrorCode::ExecutorWrongOwner);
  CHECK(release_error->code == model::DomainErrorCode::ExecutorWrongOwner);

  // ++++++++++++++++++++++++++++++++++++++++
  // Explicit release permits a successor thread to bind and release the same executor.
  REQUIRE(executor.release_from_current_thread());
  std::optional<model::DomainError> handoff_error;
  std::thread successor{[&executor, &handoff_error] {
    const auto binding = executor.bind_to_current_thread();
    if (!binding) {
      handoff_error.emplace(binding.error());
      return;
    }
    const auto release = executor.release_from_current_thread();
    if (!release) {
      handoff_error.emplace(release.error());
    }
  }};
  successor.join();
  CHECK_FALSE(handoff_error.has_value());
  CHECK_FALSE(executor.snapshot().owner_bound);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// An active handler rejects recursive run and drive while allowing ordinary admission only for a
// later owner turn.
TEST_CASE("active turns reject reentry and defer owner-side admission", "[runtime][executor]") {
  model::DeterministicClockProvider clock;
  runtime::SerializedExecutor executor{1U, clock};
  runtime::DeterministicExecutorDriver driver{executor};
  FixedRecorder recorder;
  const auto follow_up = runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 2});
  ReentryObservation observation{&executor,
                                 &follow_up,
                                 false,
                                 false,
                                 model::DomainErrorCode::InvalidValue,
                                 model::DomainErrorCode::InvalidValue,
                                 std::nullopt};
  REQUIRE(
      executor.try_admit(runtime::WorkItem::make<&observe_reentry>(ReentryCommand{&observation})));
  REQUIRE(driver.bind_to_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
  // Nested progression fails before mutable work while admission receives attempt two.
  const auto first = driver.run_one();
  REQUIRE(first);
  REQUIRE(first.value().has_value());
  CHECK(observation.nested_run_rejected);
  CHECK(observation.nested_drive_rejected);
  CHECK(observation.nested_run_error == model::DomainErrorCode::ExecutorReentryDetected);
  CHECK(observation.nested_drive_error == model::DomainErrorCode::ExecutorReentryDetected);
  REQUIRE(observation.follow_up_decision.has_value());
  CHECK(observation.follow_up_decision->outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(observation.follow_up_decision->attempt_ordinal.value() == 2U);
  CHECK(recorder.size == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A separate call stack executes the deferred command exactly once.
  const auto second = driver.run_one();
  REQUIRE(second);
  REQUIRE(second.value().has_value());
  CHECK(second.value()->attempt_ordinal.value() == 2U);
  REQUIRE(recorder.size == 1U);
  CHECK(recorder.values[0] == 2);
  REQUIRE(driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Drive enforces its explicit bound across the same run_one implementation and exposes remaining
// commands and fences independently.
TEST_CASE("deterministic drive stops at the requested turn bound", "[runtime][executor]") {
  model::DeterministicClockProvider clock;
  constexpr std::size_t maximum_drive_turns = 2U;
  runtime::SerializedExecutor executor{3U, clock, maximum_drive_turns};
  runtime::DeterministicExecutorDriver driver{executor};
  FixedRecorder recorder;
  for (int value = 1; value <= 3; ++value) {
    REQUIRE(executor.try_admit(
        runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, value})));
  }
  REQUIRE(driver.bind_to_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
  // Zero does no work and a call exactly at the policy bound completes two turns.
  const auto zero = driver.drive(0U);
  const auto two = driver.drive(2U);
  REQUIRE(zero);
  REQUIRE(two);
  CHECK(zero.value() == runtime::DriveReport{0U, 3U, 0U, 3U, maximum_drive_turns, 0U});
  CHECK(two.value() == runtime::DriveReport{2U, 1U, 0U, 3U, maximum_drive_turns, 2U});

  // ++++++++++++++++++++++++++++++++++++++++
  // Exact-plus-one fails before consuming the remaining command; a later in-bound call drains it.
  const auto above_bound = driver.drive(3U);
  REQUIRE_FALSE(above_bound);
  CHECK(above_bound.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidRuntimePolicy,
                                     "maximum_drive_turns"));
  CHECK(executor.snapshot().pending_commands == 1U);
  CHECK(executor.snapshot().completed_turns == 2U);
  const auto remainder = driver.drive(1U);
  REQUIRE(remainder);
  CHECK(remainder.value() == runtime::DriveReport{1U, 0U, 0U, 3U, maximum_drive_turns, 3U});
  REQUIRE(recorder.size == 3U);
  REQUIRE(driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Admission counter exhaustion fails before assigning another attempt and remains a stable
// terminal fault rather than degrading into ordinary Closed.
TEST_CASE("attempt exhaustion fails closed before another decision", "[runtime][executor]") {
  model::DeterministicClockProvider clock;
  const auto maximum =
      model::AdmissionOrdinal::from_value(std::numeric_limits<std::uint64_t>::max()).value();
  runtime::SerializedExecutor executor{
      1U, clock, raw_drive_limit,
      runtime::ExecutorCounterSeed{maximum, std::nullopt, std::nullopt}};
  FixedRecorder recorder;
  const auto work = runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 1});

  // ++++++++++++++++++++++++++++++++++++++++
  // No ordinary decision is published, and later attempts repeat the terminal counter failure.
  const auto exhausted = executor.try_admit(work);
  const auto repeated = executor.try_admit(work);
  REQUIRE_FALSE(exhausted);
  REQUIRE_FALSE(repeated);
  CHECK(exhausted.error().code == model::DomainErrorCode::ExecutorCounterExhausted);
  CHECK(exhausted.error().context.field == "admission_ordinal");
  CHECK(repeated.error() == exhausted.error());
  CHECK(executor.snapshot() ==
        runtime::QueueSnapshot{0U, 0U, 0U, 1U, 0U, raw_drive_limit, 0U, true, true, false, false});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Receive exhaustion is checked only for an otherwise accepted command but still occurs before
// its attempt ordinal, timestamp, or ring slot can be committed.
TEST_CASE("receive exhaustion fails an otherwise accepted attempt atomically",
          "[runtime][executor]") {
  model::DeterministicClockProvider clock{12U};
  const auto last_attempt = model::AdmissionOrdinal::from_value(41U).value();
  const auto maximum_receive =
      model::ReceiveSequence::from_value(std::numeric_limits<std::uint64_t>::max()).value();
  runtime::SerializedExecutor executor{
      1U, clock, raw_drive_limit,
      runtime::ExecutorCounterSeed{last_attempt, maximum_receive, std::nullopt}};
  FixedRecorder recorder;

  // ++++++++++++++++++++++++++++++++++++++++
  // Receive exhaustion produces no attempt 42 decision and leaves the command queue empty.
  const auto exhausted =
      executor.try_admit(runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 1}));
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == model::DomainErrorCode::ExecutorCounterExhausted);
  CHECK(exhausted.error().context.field == "receive_sequence");
  CHECK(executor.snapshot().pending_commands == 0U);
  CHECK(executor.snapshot().faulted);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Turn exhaustion is detected while the globally oldest candidate remains intact and its handler
// has not run.
TEST_CASE("turn exhaustion retains the selected head and fails closed", "[runtime][executor]") {
  model::DeterministicClockProvider clock;
  const auto maximum_turn =
      model::TurnOrdinal::from_value(std::numeric_limits<std::uint64_t>::max()).value();
  runtime::SerializedExecutor executor{
      1U, clock, raw_drive_limit,
      runtime::ExecutorCounterSeed{std::nullopt, std::nullopt, maximum_turn}};
  runtime::DeterministicExecutorDriver driver{executor};
  FixedRecorder recorder;
  REQUIRE(executor.try_admit(runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 1})));
  REQUIRE(driver.bind_to_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
  // The head remains pending and no mutable handler effect occurs after exhaustion.
  const auto exhausted = driver.run_one();
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == model::DomainErrorCode::ExecutorCounterExhausted);
  CHECK(exhausted.error().context.field == "turn_ordinal");
  CHECK(executor.snapshot().pending_commands == 1U);
  CHECK(executor.snapshot().faulted);
  CHECK(recorder.size == 0U);
  REQUIRE(driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A processing clock regression is terminal and is detected before candidate removal or owner
// handler invocation.
TEST_CASE("processing clock regression retains the head and fails closed", "[runtime][executor]") {
  ScriptedClockProvider clock{{10U, 9U, 9U, 9U}};
  runtime::SerializedExecutor executor{1U, clock};
  runtime::DeterministicExecutorDriver driver{executor};
  FixedRecorder recorder;
  const auto work = runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 7});
  REQUIRE(executor.try_admit(work));
  REQUIRE(driver.bind_to_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
  // Regression closes before the head moves; subsequent admission repeats the terminal failure.
  const auto regressed = driver.run_one();
  const auto later = executor.try_admit(work);
  REQUIRE_FALSE(regressed);
  REQUIRE_FALSE(later);
  CHECK(regressed.error() ==
        model::DomainError::at_field(model::DomainErrorCode::ExecutorClockRegression,
                                     "executor_processing_clock"));
  CHECK(later.error() == regressed.error());
  CHECK(executor.snapshot().pending_commands == 1U);
  CHECK(executor.snapshot().faulted);
  CHECK(recorder.size == 0U);
  REQUIRE(driver.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A later accepted-admission clock regression fails before assigning its attempt or receive
// ordinal and cannot disturb the earlier accepted prefix.
TEST_CASE("admission clock regression preserves the earlier accepted prefix",
          "[runtime][executor]") {
  ScriptedClockProvider clock{{10U, 9U, 9U, 9U}};
  runtime::SerializedExecutor executor{2U, clock};
  FixedRecorder recorder;
  const auto work = runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 1});
  const auto first = executor.try_admit(work);
  const auto regressed = executor.try_admit(work);
  REQUIRE(first);
  REQUIRE_FALSE(regressed);
  CHECK(first.value().attempt_ordinal.value() == 1U);
  CHECK(regressed.error().code == model::DomainErrorCode::ExecutorClockRegression);
  CHECK(executor.snapshot().pending_commands == 1U);
  CHECK(executor.snapshot().faulted);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Synchronized stop observed during an open active turn prevents the dedicated owner from starting
// a queued successor after the current run-to-completion stack returns.
TEST_CASE("dedicated stop gates the next open-ingress turn", "[runtime][executor]") {
  model::SystemClockProvider clock;
  runtime::SerializedExecutor executor{2U, clock};
  std::atomic_size_t completed{0U};
  std::latch first_started{1U};
  std::latch release_first{1U};
  const auto work = runtime::WorkItem::make<&wait_for_stop_gate>(
      StopGateCommand{&completed, &first_started, &release_first});
  REQUIRE(executor.try_admit(work));
  REQUIRE(executor.try_admit(work));
  runtime::DedicatedExecutorDriver dedicated{executor};
  REQUIRE(dedicated.started_successfully());
  first_started.wait();

  // ++++++++++++++++++++++++++++++++++++++++
  // Stop is published while turn one is active; turn two remains pending after owner release.
  dedicated.request_stop();
  release_first.count_down();
  dedicated.wait_until_stopped();
  CHECK(completed.load() == 1U);
  CHECK_FALSE(dedicated.terminal_error().has_value());
  CHECK(executor.snapshot().pending_commands == 1U);
  CHECK_FALSE(executor.snapshot().closed);

  // ++++++++++++++++++++++++++++++++++++++++
  // Stop belongs to that driver, so a deliberate later manual owner may process the retained work.
  runtime::DeterministicExecutorDriver manual{executor};
  REQUIRE(manual.bind_to_current_thread());
  REQUIRE(manual.run_one());
  CHECK(completed.load() == 2U);
  REQUIRE(manual.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Closed-prefix drainage takes precedence over a synchronized stop requested during an active
// owner turn.
TEST_CASE("dedicated stop preserves closed-prefix drainage", "[runtime][executor]") {
  model::SystemClockProvider clock;
  runtime::SerializedExecutor executor{2U, clock};
  std::atomic_size_t completed{0U};
  std::latch first_started{1U};
  std::latch release_first{1U};
  const auto work = runtime::WorkItem::make<&wait_for_stop_gate>(
      StopGateCommand{&completed, &first_started, &release_first});
  REQUIRE(executor.try_admit(work));
  REQUIRE(executor.try_admit(work));
  runtime::DedicatedExecutorDriver dedicated{executor};
  REQUIRE(dedicated.started_successfully());
  first_started.wait();

  // ++++++++++++++++++++++++++++++++++++++++
  // Once closed, both accepted turns remain the immutable prefix even though stop is also set.
  executor.close();
  dedicated.request_stop();
  release_first.count_down();
  dedicated.wait_until_stopped();
  CHECK(completed.load() == 2U);
  CHECK_FALSE(dedicated.terminal_error().has_value());
  CHECK(executor.snapshot().pending_commands == 0U);
  CHECK(executor.snapshot().closed);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Dedicated progression must publish a successful overlapping turn before stopping with the
// producer fault that the handler triggered.
TEST_CASE("dedicated owner reports applied turn before overlapping terminal fault",
          "[runtime][executor]") {
  model::SystemClockProvider clock;
  const auto penultimate_attempt =
      model::AdmissionOrdinal::from_value(std::numeric_limits<std::uint64_t>::max() - 1U).value();
  runtime::SerializedExecutor executor{
      1U, clock, raw_drive_limit,
      runtime::ExecutorCounterSeed{penultimate_attempt, std::nullopt, std::nullopt}};
  FixedRecorder recorder;
  const auto follow_up = runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 2});
  TerminalOverlapObservation observation{&executor, &follow_up, 0U, std::nullopt, std::nullopt};
  REQUIRE(executor.try_admit(runtime::WorkItem::make<&apply_then_exhaust_admission>(
      TerminalOverlapCommand{&observation})));

  // ++++++++++++++++++++++++++++++++++++++++
  // No sleeps are needed: the worker's stopped publication follows handler completion and terminal
  // propagation through the driver's condition-variable lifecycle.
  runtime::DedicatedExecutorDriver dedicated{executor};
  REQUIRE(dedicated.started_successfully());
  dedicated.wait_until_stopped();
  CHECK(observation.applied == 1U);
  REQUIRE(observation.context.has_value());
  CHECK(observation.context->turn_ordinal.value() == 1U);
  REQUIRE(observation.nested_error.has_value());
  REQUIRE(dedicated.terminal_error().has_value());
  CHECK(dedicated.terminal_error().value() == observation.nested_error.value());
  REQUIRE(dedicated.last_turn_report().has_value());
  CHECK(dedicated.last_turn_report()->turn_ordinal.value() == 1U);
  CHECK(dedicated.last_turn_report()->attempt_ordinal.value() ==
        std::numeric_limits<std::uint64_t>::max());
  CHECK(executor.snapshot().completed_turns == 1U);
  CHECK(executor.snapshot().faulted);
  CHECK_FALSE(executor.snapshot().owner_bound);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A producer-side terminal fault can wake the dedicated owner directly from its wait predicate;
// driver status must still publish the executor's stable cause.
TEST_CASE("dedicated wait exit propagates executor terminal fault", "[runtime][executor]") {
  model::SystemClockProvider clock;
  const auto maximum =
      model::AdmissionOrdinal::from_value(std::numeric_limits<std::uint64_t>::max()).value();
  runtime::SerializedExecutor executor{
      1U, clock, raw_drive_limit,
      runtime::ExecutorCounterSeed{maximum, std::nullopt, std::nullopt}};
  runtime::DedicatedExecutorDriver dedicated{executor};
  REQUIRE(dedicated.started_successfully());
  FixedRecorder recorder;

  // ++++++++++++++++++++++++++++++++++++++++
  // Exhaustion closes and wakes without a runnable head, then appears identically in driver state.
  const auto exhausted =
      executor.try_admit(runtime::WorkItem::make<&record_value>(RecordCommand{&recorder, 1}));
  REQUIRE_FALSE(exhausted);
  dedicated.wait_until_stopped();
  REQUIRE(dedicated.terminal_error().has_value());
  CHECK(dedicated.terminal_error().value() == exhausted.error());
  CHECK_FALSE(executor.snapshot().owner_bound);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A dedicated owner wakes for a fence that consumes no FIFO capacity, invokes the same run_one
// handler, and releases ownership after closed-empty drainage.
TEST_CASE("dedicated owner wakes for fences and exits after close drainage",
          "[runtime][executor]") {
  model::SystemClockProvider clock;
  FixedRecorder order;
  FenceRecorder fences{order};
  std::latch fence_completed{1U};
  fences.set_completion_latch(fence_completed);
  runtime::SerializedExecutor executor{0U, 1U, clock, fences};
  runtime::DedicatedExecutorDriver driver{executor};
  REQUIRE(driver.started_successfully());

  // ++++++++++++++++++++++++++++++++++++++++
  // Capacity rejection creates the sole runnable fence and wakes the already waiting owner.
  const auto source = model::MarketSourceOrdinal::initial();
  FixedRecorder ignored;
  const auto rejected = executor.try_admit(
      runtime::WorkItem::make<&record_value>(RecordCommand{&ignored, 1}), source);
  REQUIRE(rejected);
  CHECK(rejected.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(rejected.value().discontinuity_recorded);
  fence_completed.wait();
  CHECK(fences.size() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Closing the now-empty executor wakes the owner to release and publish stable completion.
  executor.close();
  driver.wait_until_stopped();
  CHECK_FALSE(driver.running());
  CHECK_FALSE(driver.terminal_error().has_value());
  REQUIRE(driver.last_turn_report().has_value());
  CHECK(driver.last_turn_report()->kind == runtime::TurnKind::SourceDiscontinuity);
  CHECK_FALSE(driver.last_turn_report()->queue_age.has_value());
  REQUIRE(driver.last_turn_report()->discontinuity.has_value());
  CHECK(driver.last_turn_report()->discontinuity->source_ordinal == source);
  CHECK(executor.snapshot() ==
        runtime::QueueSnapshot{0U, 0U, 0U, 0U, 1U, raw_drive_limit, 1U, true, false, false, false});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Dedicated closure drains accepted work through the shared processor before publishing stopped
// state and supports a later explicit manual owner handoff.
TEST_CASE("dedicated owner drains accepted work and releases ownership", "[runtime][executor]") {
  model::SystemClockProvider clock;
  runtime::SerializedExecutor executor{2U, clock};
  std::atomic_size_t completed{0U};
  std::latch completed_latch{2U};
  const auto work =
      runtime::WorkItem::make<&count_completion>(CompletionCommand{&completed, &completed_latch});
  REQUIRE(executor.try_admit(work));
  REQUIRE(executor.try_admit(work));
  executor.close();

  // ++++++++++++++++++++++++++++++++++++++++
  // The dedicated owner binds, drains both commands, and exits only at closed-empty.
  runtime::DedicatedExecutorDriver dedicated{executor};
  REQUIRE(dedicated.started_successfully());
  completed_latch.wait();
  dedicated.wait_until_stopped();
  CHECK_FALSE(dedicated.running());
  CHECK_FALSE(dedicated.terminal_error().has_value());
  CHECK(completed.load() == 2U);
  const auto last_report = dedicated.last_turn_report();
  REQUIRE(last_report.has_value());
  CHECK(last_report->kind == runtime::TurnKind::Command);
  CHECK(last_report->turn_ordinal.value() == 2U);
  CHECK(last_report->receive_sequence == model::ReceiveSequence::from_value(2U).value());
  CHECK(last_report->queue_age.has_value());
  CHECK(executor.snapshot() ==
        runtime::QueueSnapshot{0U, 0U, 0U, 2U, 0U, raw_drive_limit, 2U, true, false, false, false});

  // ++++++++++++++++++++++++++++++++++++++++
  // Released ownership can be rebound even though admission closure remains permanent.
  runtime::DeterministicExecutorDriver manual{executor};
  REQUIRE(manual.bind_to_current_thread());
  const auto empty = manual.run_one();
  REQUIRE(empty);
  CHECK_FALSE(empty.value().has_value());
  REQUIRE(manual.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Dedicated startup reports owner conflict instead of stealing an existing deterministic binding.
TEST_CASE("dedicated owner reports binding conflict", "[runtime][executor]") {
  model::SystemClockProvider clock;
  runtime::SerializedExecutor executor{1U, clock};
  REQUIRE(executor.bind_to_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
  // Construction waits for WrongOwner publication and stable stopped state.
  runtime::DedicatedExecutorDriver dedicated{executor};
  CHECK_FALSE(dedicated.started_successfully());
  dedicated.wait_until_stopped();
  REQUIRE(dedicated.terminal_error().has_value());
  CHECK(dedicated.terminal_error()->code == model::DomainErrorCode::ExecutorWrongOwner);
  CHECK(executor.snapshot().owner_bound);
  REQUIRE(executor.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
