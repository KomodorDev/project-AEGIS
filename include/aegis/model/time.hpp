// Purpose: define type-distinct time, sequence, session, and revision values plus injected
// monotonic clocks; cross-clock arithmetic is available only through named checked operations.

#pragma once

#include "aegis/model/integer_input.hpp"
#include "aegis/model/result.hpp"

#include <chrono>
#include <compare>
#include <concepts>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace aegis::model {
namespace detail {

// ########################################################################
// Source timestamps use a distinct incomplete tag.
struct SourceTimestampTag;

// ########################################################################
// Receive timestamps use a distinct incomplete tag.
struct ReceiveTimestampTag;

// ########################################################################
// Processing timestamps use a distinct incomplete tag.
struct ProcessingTimestampTag;

// ########################################################################
// Interesting syntax: tag-parameterized storage produces unrelated source, receive, and processing
// timestamp types, so comparisons cannot accidentally cross wall-clock and monotonic domains.
template <typename Tag> class Timestamp {
public:

  // --------------------------------------------------------
  // Interesting syntax: CheckedUnsignedIntegerInput admits unsigned char and wider unsigned
  // standard integers, but excludes signed, bool, enum, floating, and plain/wide/Unicode character
  // sources before this non-fallible cast.
  template <CheckedUnsignedIntegerInput Value>
    requires(sizeof(UnqualifiedIntegerInput<Value>) <= sizeof(std::uint64_t))
  explicit constexpr Timestamp(Value nanoseconds) noexcept
      : nanoseconds_{static_cast<std::uint64_t>(nanoseconds)} {}

  // --------------------------------------------------------
  // Return the exact unsigned nanosecond count in this timestamp's nominal clock domain.
  [[nodiscard]] constexpr std::uint64_t nanoseconds() const noexcept { return nanoseconds_; }

  // --------------------------------------------------------
  // Compare timestamps only within the same nominal clock domain.
  friend constexpr bool operator==(Timestamp, Timestamp) = default;
  friend constexpr auto operator<=>(Timestamp, Timestamp) = default;

  // --------------------------------------------------------
private:
  std::uint64_t nanoseconds_;
};

// ########################################################################
// Session epochs report failures through a stable field name.
struct SessionEpochTag {
  static constexpr std::string_view field = "session_epoch";
};

// ########################################################################
// Sequence numbers report failures through a distinct stable field name.
struct SequenceNumberTag {
  static constexpr std::string_view field = "sequence_number";
};

// ########################################################################
// Runtime and market ordinals are one-based and retain subsystem-specific exhaustion errors while
// sharing one checked representation.
#define AEGIS_ORDINAL_TAG(TagName, Field, ExhaustionCode)                                          \
  struct TagName {                                                                                 \
    static constexpr std::string_view field = Field;                                               \
    static constexpr DomainErrorCode exhaustion_code = DomainErrorCode::ExhaustionCode;            \
  }

// ########################################################################
// Configured market sources receive a canonical one-based runtime-policy position.
AEGIS_ORDINAL_TAG(MarketSourceOrdinalTag, "market_source_ordinal", ExecutorCounterExhausted);

// ########################################################################
// Every ordinary admission attempt receives a one-based position, including rejected attempts.
AEGIS_ORDINAL_TAG(AdmissionOrdinalTag, "admission_ordinal", ExecutorCounterExhausted);

// ########################################################################
// Only accepted work receives a distinct one-based local receive sequence.
AEGIS_ORDINAL_TAG(ReceiveSequenceTag, "receive_sequence", ExecutorCounterExhausted);

// ########################################################################
// Completed owner turns receive positions independent from admission attempts.
AEGIS_ORDINAL_TAG(TurnOrdinalTag, "turn_ordinal", ExecutorCounterExhausted);

// ########################################################################
// Strategy callbacks use a separate sequence so their order cannot be confused with owner turns.
AEGIS_ORDINAL_TAG(CallbackOrdinalTag, "callback_ordinal", CallbackCounterExhausted);

// ########################################################################
// Submission attempts receive a distinct bounded position before canonical evidence preflight.
AEGIS_ORDINAL_TAG(SubmissionAttemptIdTag, "submission_attempt_id", CounterExhausted);

// ########################################################################
// Owner-local route projections receive a stable canonical startup position.
AEGIS_ORDINAL_TAG(RouteOrdinalTag, "route_ordinal", CounterExhausted);

// ########################################################################
// Held risk reservations reuse the creating attempt value behind a distinct nominal type.
AEGIS_ORDINAL_TAG(ReservationIdTag, "reservation_id", CounterExhausted);

// ########################################################################
// Encoder calls advance only when exact fake encoding is reached.
AEGIS_ORDINAL_TAG(EncoderInvocationOrdinalTag, "encoder_invocation_ordinal", CounterExhausted);

// ########################################################################
// Initiator calls advance independently from outer attempts and encoder calls.
AEGIS_ORDINAL_TAG(InitiatorInvocationOrdinalTag, "initiator_invocation_ordinal", CounterExhausted);

// ########################################################################
// Only accepted-slot copies receive a fake-write identity.
AEGIS_ORDINAL_TAG(FakeWriteOrdinalTag, "fake_write_ordinal", CounterExhausted);

// ########################################################################
// Every accepted full snapshot starts a one-based book generation.
AEGIS_ORDINAL_TAG(BookGenerationTag, "book_generation", MarketCounterExhausted);

// ########################################################################
// Every committed snapshot or delta advances the globally monotonic book revision.
AEGIS_ORDINAL_TAG(BookRevisionTag, "book_revision", MarketCounterExhausted);

// ########################################################################
#undef AEGIS_ORDINAL_TAG

// ########################################################################
// Session epochs and source sequences allow zero where their protocols require it, but increment
// fails before unsigned wrap and reports the tag-specific stable field.
template <typename Tag> class CheckedUnsigned {
public:

  // --------------------------------------------------------
  // Epoch and sequence construction uses the same no-narrowing rule as timestamps before storing a
  // uint64_t value.
  template <CheckedUnsignedIntegerInput Value>
    requires(sizeof(UnqualifiedIntegerInput<Value>) <= sizeof(std::uint64_t))
  explicit constexpr CheckedUnsigned(Value value) noexcept
      : value_{static_cast<std::uint64_t>(value)} {}

  // --------------------------------------------------------
  // Return the exact stored protocol counter.
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  // --------------------------------------------------------
  // Advance exactly once, failing before unsigned wrap with the tag-specific field.
  [[nodiscard]] Result<CheckedUnsigned> next() const {

    // ++++++++++++++++++++++++++++++++++++++++
    // Reject the terminal value before arithmetic can wrap.
    if (value_ == std::numeric_limits<std::uint64_t>::max()) {
      return Result<CheckedUnsigned>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, std::string{Tag::field}));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Publish the representable successor in the same nominal counter domain.
    return Result<CheckedUnsigned>::success(CheckedUnsigned{value_ + 1U});

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Compare counters only within the same nominal tag instantiation.
  friend constexpr bool operator==(CheckedUnsigned, CheckedUnsigned) = default;
  friend constexpr auto operator<=>(CheckedUnsigned, CheckedUnsigned) = default;

  // --------------------------------------------------------
private:
  std::uint64_t value_;
};

// ########################################################################
// One-based ordinals reject absence at their factory boundary and fail before increment wrap. They
// remain nominally distinct even when several runtime sequences advance together in one turn.
template <typename Tag> class OneBasedOrdinal {
public:

  // --------------------------------------------------------
  // Accept portable integer inputs only when they are positive and exactly representable.
  template <CheckedIntegerInput Value>
  [[nodiscard]] static Result<OneBasedOrdinal> from_value(Value value) {

    // ++++++++++++++++++++++++++++++++++++++++
    // Zero is reserved for absence in primitive trace encodings; negative and wide values also
    // fail.
    if (!std::in_range<std::uint64_t>(value) || value == 0) {
      return Result<OneBasedOrdinal>::failure(
          DomainError::at_field(DomainErrorCode::InvalidValue, std::string{Tag::field}));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Publish only a positive exactly represented ordinal.
    return Result<OneBasedOrdinal>::success(OneBasedOrdinal{static_cast<std::uint64_t>(value)});

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Return the first assigned position without a fallible construction path.
  [[nodiscard]] static constexpr OneBasedOrdinal initial() noexcept { return OneBasedOrdinal{1U}; }

  // --------------------------------------------------------
  // Return the exact one-based position.
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  // --------------------------------------------------------
  // Advance exactly once or report the tag's stable subsystem-specific exhaustion code.
  [[nodiscard]] Result<OneBasedOrdinal> next() const {

    // ++++++++++++++++++++++++++++++++++++++++
    // Reject the terminal value before unsigned arithmetic can wrap to the absence sentinel.
    if (value_ == std::numeric_limits<std::uint64_t>::max()) {
      return Result<OneBasedOrdinal>::failure(
          DomainError::at_field(Tag::exhaustion_code, std::string{Tag::field}));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Publish the representable successor in the same nominal domain.
    return Result<OneBasedOrdinal>::success(OneBasedOrdinal{value_ + 1U});

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Compare only ordinals from the same nominal domain.
  friend constexpr bool operator==(OneBasedOrdinal, OneBasedOrdinal) = default;
  friend constexpr auto operator<=>(OneBasedOrdinal, OneBasedOrdinal) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Restrict raw construction to validated factories and checked successor generation.
  explicit constexpr OneBasedOrdinal(std::uint64_t value) noexcept : value_{value} {}

  // --------------------------------------------------------
  std::uint64_t value_;
};

// ########################################################################
// Revision tags supply stable error fields while keeping each configuration dimension nominally
// distinct from every other revision kind.
#define AEGIS_REVISION_TAG(TagName, Field)                                                         \
  struct TagName {                                                                                 \
    static constexpr std::string_view field = Field;                                               \
  }

// ########################################################################
// Configuration revisions report failures through the complete-snapshot field.
AEGIS_REVISION_TAG(ConfigurationRevisionTag, "configuration_revision");

// ########################################################################
// Organization revisions report failures through their section field.
AEGIS_REVISION_TAG(OrganizationRevisionTag, "organization_revision");

// ########################################################################
// Instrument metadata revisions report failures through their section field.
AEGIS_REVISION_TAG(InstrumentMetadataRevisionTag, "instrument_metadata_revision");

// ########################################################################
// Strategy configuration revisions report failures through their section field.
AEGIS_REVISION_TAG(StrategyConfigurationRevisionTag, "strategy_configuration_revision");

// ########################################################################
// Subscription revisions report failures through their section field.
AEGIS_REVISION_TAG(SubscriptionRevisionTag, "subscription_revision");

// ########################################################################
// Route revisions report failures through their section field.
AEGIS_REVISION_TAG(RouteRevisionTag, "route_revision");

// ########################################################################
// Immutable M3 risk snapshots receive a distinct nonzero policy revision.
AEGIS_REVISION_TAG(RiskPolicyRevisionTag, "risk_policy_revision");

// ########################################################################
#undef AEGIS_REVISION_TAG

// ########################################################################
// Installed revisions start at one: zero represents absence, and neither construction nor
// increment may silently narrow or wrap.
template <typename Tag> class Revision {
public:

  // --------------------------------------------------------
  // CheckedIntegerInput admits signed/unsigned char and wider standard integers. A fallible factory
  // keeps signed input intact until range and nonzero checks can report the revision tag's field.
  template <CheckedIntegerInput Value>
  [[nodiscard]] static Result<Revision> from_value(Value value) {

    // ++++++++++++++++++++++++++++++++++++++++
    // Reject absence, negative input, and values outside the stored unsigned width.
    if (!std::in_range<std::uint64_t>(value) || value == 0) {
      return Result<Revision>::failure(
          DomainError::at_field(DomainErrorCode::InvalidRevision, std::string{Tag::field}));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Publish only a positive representable revision.
    return Result<Revision>::success(Revision{static_cast<std::uint64_t>(value)});

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Return the first installed revision without a fallible construction path.
  [[nodiscard]] static constexpr Revision initial() noexcept { return Revision{1U}; }

  // --------------------------------------------------------
  // Return the exact positive stored revision number.
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  // --------------------------------------------------------
  // Advance exactly once, failing before unsigned wrap with the tag-specific field.
  [[nodiscard]] Result<Revision> next() const {

    // ++++++++++++++++++++++++++++++++++++++++
    // Reject the terminal value before arithmetic can wrap.
    if (value_ == std::numeric_limits<std::uint64_t>::max()) {
      return Result<Revision>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, std::string{Tag::field}));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Publish the representable successor in the same nominal revision domain.
    return Result<Revision>::success(Revision{value_ + 1U});

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Compare revisions only within the same nominal tag instantiation.
  friend constexpr bool operator==(Revision, Revision) = default;
  friend constexpr auto operator<=>(Revision, Revision) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Restrict fixed-width construction to validated factories and internal successor generation.
  explicit constexpr Revision(std::uint64_t value) noexcept : value_{value} {}

  // --------------------------------------------------------
  std::uint64_t value_;
};

// ########################################################################
} // namespace detail

// ########################################################################
// Public timestamp names expose the clock-domain distinction without exposing their tag machinery.
using SourceTimestamp = detail::Timestamp<detail::SourceTimestampTag>;
using ReceiveTimestamp = detail::Timestamp<detail::ReceiveTimestampTag>;
using ProcessingTimestamp = detail::Timestamp<detail::ProcessingTimestampTag>;

// ########################################################################
// Elapsed time has its own type so a duration cannot be supplied where an absolute timestamp is
// required. The named subtraction rejects reversed receive/processing order.
class ElapsedNanoseconds {
public:

  // --------------------------------------------------------
  // A duration constructor is intentionally unsigned-only because invalid signed input has no
  // result channel in which to return an error.
  template <detail::CheckedUnsignedIntegerInput Value>
    requires(sizeof(detail::UnqualifiedIntegerInput<Value>) <= sizeof(std::uint64_t))
  explicit constexpr ElapsedNanoseconds(Value value) noexcept
      : value_{static_cast<std::uint64_t>(value)} {}

  // --------------------------------------------------------
  // Return the exact unsigned elapsed duration.
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  // --------------------------------------------------------
  // Compare elapsed durations structurally and numerically.
  friend constexpr bool operator==(ElapsedNanoseconds, ElapsedNanoseconds) = default;
  friend constexpr auto operator<=>(ElapsedNanoseconds, ElapsedNanoseconds) = default;

  // --------------------------------------------------------
private:
  std::uint64_t value_;
};

// ########################################################################

// --------------------------------------------------------
// Subtract receive time from later processing time without mixing timestamp domains implicitly.
[[nodiscard]] Result<ElapsedNanoseconds> processing_delay(ProcessingTimestamp processing,
                                                          ReceiveTimestamp receive);

// --------------------------------------------------------

// ########################################################################
// Epochs, sequences, and revisions share checked mechanics but remain unrelated public types.
using SessionEpoch = detail::CheckedUnsigned<detail::SessionEpochTag>;
using SequenceNumber = detail::CheckedUnsigned<detail::SequenceNumberTag>;

// ########################################################################
// Runtime, source, callback, and book positions share one checked mechanism but cannot be mixed.
using MarketSourceOrdinal = detail::OneBasedOrdinal<detail::MarketSourceOrdinalTag>;
using AdmissionOrdinal = detail::OneBasedOrdinal<detail::AdmissionOrdinalTag>;
using ReceiveSequence = detail::OneBasedOrdinal<detail::ReceiveSequenceTag>;
using TurnOrdinal = detail::OneBasedOrdinal<detail::TurnOrdinalTag>;
using CallbackOrdinal = detail::OneBasedOrdinal<detail::CallbackOrdinalTag>;
using SubmissionAttemptId = detail::OneBasedOrdinal<detail::SubmissionAttemptIdTag>;
using RouteOrdinal = detail::OneBasedOrdinal<detail::RouteOrdinalTag>;
using ReservationId = detail::OneBasedOrdinal<detail::ReservationIdTag>;
using EncoderInvocationOrdinal = detail::OneBasedOrdinal<detail::EncoderInvocationOrdinalTag>;
using InitiatorInvocationOrdinal = detail::OneBasedOrdinal<detail::InitiatorInvocationOrdinalTag>;
using FakeWriteOrdinal = detail::OneBasedOrdinal<detail::FakeWriteOrdinalTag>;
using BookGeneration = detail::OneBasedOrdinal<detail::BookGenerationTag>;
using BookRevision = detail::OneBasedOrdinal<detail::BookRevisionTag>;

// ########################################################################
// Revision aliases keep each configuration section nominally distinct over shared checked
// mechanics.
using ConfigurationRevision = detail::Revision<detail::ConfigurationRevisionTag>;
using OrganizationRevision = detail::Revision<detail::OrganizationRevisionTag>;
using InstrumentMetadataRevision = detail::Revision<detail::InstrumentMetadataRevisionTag>;
using StrategyConfigurationRevision = detail::Revision<detail::StrategyConfigurationRevisionTag>;
using SubscriptionRevision = detail::Revision<detail::SubscriptionRevisionTag>;
using RouteRevision = detail::Revision<detail::RouteRevisionTag>;
using RiskPolicyRevision = detail::Revision<detail::RiskPolicyRevisionTag>;

// ########################################################################
// Interesting syntax: the non-virtual public API wraps a private raw-counter hook, ensuring every
// implementation returns the correct nominal timestamp type while callers inject one capability.
class ClockProvider {
public:

  // --------------------------------------------------------
  // Keep clock capabilities non-copyable and non-movable while allowing safe polymorphic cleanup.
  ClockProvider() = default;
  ClockProvider(const ClockProvider&) = delete;
  ClockProvider& operator=(const ClockProvider&) = delete;
  ClockProvider(ClockProvider&&) = delete;
  ClockProvider& operator=(ClockProvider&&) = delete;
  virtual ~ClockProvider() = default;

  // --------------------------------------------------------
  // Wrap the provider's monotonic count in the receive-time nominal domain.
  [[nodiscard]] ReceiveTimestamp receive_now() noexcept {
    return ReceiveTimestamp{monotonic_nanoseconds()};
  }

  // --------------------------------------------------------
  // Wrap the provider's monotonic count in the processing-time nominal domain.
  [[nodiscard]] ProcessingTimestamp processing_now() noexcept {
    return ProcessingTimestamp{monotonic_nanoseconds()};
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Let concrete providers supply only a raw monotonic count behind the typed public wrappers.
  [[nodiscard]] virtual std::uint64_t monotonic_nanoseconds() noexcept = 0;

  // --------------------------------------------------------
};

// ########################################################################
// The deterministic clock is test-controlled and never advances implicitly; signed entry is
// accepted only so negative values can fail as domain errors before any unsigned conversion.
class DeterministicClockProvider final : public ClockProvider {
public:

  // --------------------------------------------------------
  // Begin a deterministic clock at zero.
  DeterministicClockProvider() noexcept = default;

  // --------------------------------------------------------
  // Initial state is non-fallible and therefore unsigned-only, matching the timestamp constructors.
  template <detail::CheckedUnsignedIntegerInput Value>
    requires(sizeof(detail::UnqualifiedIntegerInput<Value>) <= sizeof(std::uint64_t))
  explicit DeterministicClockProvider(Value initial_nanoseconds) noexcept
      : current_nanoseconds_{static_cast<std::uint64_t>(initial_nanoseconds)} {}

  // --------------------------------------------------------
  // Advance is fallible, so CheckedIntegerInput admits signed/unsigned char and wider integer
  // sources, then rejects negative values at clock_nanoseconds before checked addition.
  template <detail::CheckedIntegerInput Value>
  [[nodiscard]] Result<void> advance(Value nanoseconds) {

    // ++++++++++++++++++++++++++++++++++++++++
    // Reject negative or unrepresentable deltas before conversion.
    if (!std::in_range<std::uint64_t>(nanoseconds)) {
      return Result<void>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "clock_nanoseconds"));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Delegate only a validated fixed-width delta to the checked state transition.
    return advance_validated(static_cast<std::uint64_t>(nanoseconds));

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Add one validated delta atomically, preserving state on overflow.
  [[nodiscard]] Result<void> advance_validated(std::uint64_t nanoseconds);

  // --------------------------------------------------------
  // Expose the current deterministic count through the base provider hook.
  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override {
    return current_nanoseconds_;
  }

  // --------------------------------------------------------
  std::uint64_t current_nanoseconds_{0U};
};

// ########################################################################
// The system implementation exposes elapsed steady-clock nanoseconds from a process-local origin,
// never wall time or an epoch comparable with SourceTimestamp.
class SystemClockProvider final : public ClockProvider {
public:

  // --------------------------------------------------------
  // Capture the process-local steady-clock origin.
  SystemClockProvider() noexcept;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Return elapsed steady-clock nanoseconds from the captured origin.
  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override;

  // --------------------------------------------------------
  std::chrono::steady_clock::time_point origin_;
};

// ########################################################################
} // namespace aegis::model
