// Purpose: keep venue, monotonic, sequence, session, and revision domains type-distinct.

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

struct SourceTimestampTag;
struct ReceiveTimestampTag;
struct ProcessingTimestampTag;

template <typename Tag> class Timestamp {
public:
  template <CheckedUnsignedIntegerInput Value>
    requires(sizeof(UnqualifiedIntegerInput<Value>) <= sizeof(std::uint64_t))
  explicit constexpr Timestamp(Value nanoseconds) noexcept
      : nanoseconds_{static_cast<std::uint64_t>(nanoseconds)} {}

  [[nodiscard]] constexpr std::uint64_t nanoseconds() const noexcept { return nanoseconds_; }

  friend constexpr bool operator==(Timestamp, Timestamp) = default;
  friend constexpr auto operator<=>(Timestamp, Timestamp) = default;

private:
  std::uint64_t nanoseconds_;
};

struct SessionEpochTag {
  static constexpr std::string_view field = "session_epoch";
};
struct SequenceNumberTag {
  static constexpr std::string_view field = "sequence_number";
};

template <typename Tag> class CheckedUnsigned {
public:
  template <CheckedUnsignedIntegerInput Value>
    requires(sizeof(UnqualifiedIntegerInput<Value>) <= sizeof(std::uint64_t))
  explicit constexpr CheckedUnsigned(Value value) noexcept
      : value_{static_cast<std::uint64_t>(value)} {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  [[nodiscard]] Result<CheckedUnsigned> next() const {
    if (value_ == std::numeric_limits<std::uint64_t>::max()) {
      return Result<CheckedUnsigned>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, std::string{Tag::field}));
    }
    return Result<CheckedUnsigned>::success(CheckedUnsigned{value_ + 1U});
  }

  friend constexpr bool operator==(CheckedUnsigned, CheckedUnsigned) = default;
  friend constexpr auto operator<=>(CheckedUnsigned, CheckedUnsigned) = default;

private:
  std::uint64_t value_;
};

#define AEGIS_REVISION_TAG(TagName, Field)                                                         \
  struct TagName {                                                                                 \
    static constexpr std::string_view field = Field;                                               \
  }

AEGIS_REVISION_TAG(ConfigurationRevisionTag, "configuration_revision");
AEGIS_REVISION_TAG(OrganizationRevisionTag, "organization_revision");
AEGIS_REVISION_TAG(InstrumentMetadataRevisionTag, "instrument_metadata_revision");
AEGIS_REVISION_TAG(StrategyConfigurationRevisionTag, "strategy_configuration_revision");
AEGIS_REVISION_TAG(SubscriptionRevisionTag, "subscription_revision");
AEGIS_REVISION_TAG(RouteRevisionTag, "route_revision");

#undef AEGIS_REVISION_TAG

template <typename Tag> class Revision {
public:
  template <CheckedIntegerInput Value>
  [[nodiscard]] static Result<Revision> from_value(Value value) {
    if (!std::in_range<std::uint64_t>(value) || value == 0) {
      return Result<Revision>::failure(
          DomainError::at_field(DomainErrorCode::InvalidRevision, std::string{Tag::field}));
    }
    return Result<Revision>::success(Revision{static_cast<std::uint64_t>(value)});
  }

  [[nodiscard]] static constexpr Revision initial() noexcept { return Revision{1U}; }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  [[nodiscard]] Result<Revision> next() const {
    if (value_ == std::numeric_limits<std::uint64_t>::max()) {
      return Result<Revision>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, std::string{Tag::field}));
    }
    return Result<Revision>::success(Revision{value_ + 1U});
  }

  friend constexpr bool operator==(Revision, Revision) = default;
  friend constexpr auto operator<=>(Revision, Revision) = default;

private:
  explicit constexpr Revision(std::uint64_t value) noexcept : value_{value} {}

  std::uint64_t value_;
};

} // namespace detail

using SourceTimestamp = detail::Timestamp<detail::SourceTimestampTag>;
using ReceiveTimestamp = detail::Timestamp<detail::ReceiveTimestampTag>;
using ProcessingTimestamp = detail::Timestamp<detail::ProcessingTimestampTag>;

class ElapsedNanoseconds {
public:
  template <detail::CheckedUnsignedIntegerInput Value>
    requires(sizeof(detail::UnqualifiedIntegerInput<Value>) <= sizeof(std::uint64_t))
  explicit constexpr ElapsedNanoseconds(Value value) noexcept
      : value_{static_cast<std::uint64_t>(value)} {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(ElapsedNanoseconds, ElapsedNanoseconds) = default;
  friend constexpr auto operator<=>(ElapsedNanoseconds, ElapsedNanoseconds) = default;

private:
  std::uint64_t value_;
};

[[nodiscard]] Result<ElapsedNanoseconds> processing_delay(ProcessingTimestamp processing,
                                                          ReceiveTimestamp receive);

using SessionEpoch = detail::CheckedUnsigned<detail::SessionEpochTag>;
using SequenceNumber = detail::CheckedUnsigned<detail::SequenceNumberTag>;

using ConfigurationRevision = detail::Revision<detail::ConfigurationRevisionTag>;
using OrganizationRevision = detail::Revision<detail::OrganizationRevisionTag>;
using InstrumentMetadataRevision = detail::Revision<detail::InstrumentMetadataRevisionTag>;
using StrategyConfigurationRevision = detail::Revision<detail::StrategyConfigurationRevisionTag>;
using SubscriptionRevision = detail::Revision<detail::SubscriptionRevisionTag>;
using RouteRevision = detail::Revision<detail::RouteRevisionTag>;

class ClockProvider {
public:
  ClockProvider() = default;
  ClockProvider(const ClockProvider&) = delete;
  ClockProvider& operator=(const ClockProvider&) = delete;
  ClockProvider(ClockProvider&&) = delete;
  ClockProvider& operator=(ClockProvider&&) = delete;
  virtual ~ClockProvider() = default;

  [[nodiscard]] ReceiveTimestamp receive_now() noexcept {
    return ReceiveTimestamp{monotonic_nanoseconds()};
  }

  [[nodiscard]] ProcessingTimestamp processing_now() noexcept {
    return ProcessingTimestamp{monotonic_nanoseconds()};
  }

private:
  [[nodiscard]] virtual std::uint64_t monotonic_nanoseconds() noexcept = 0;
};

class DeterministicClockProvider final : public ClockProvider {
public:
  DeterministicClockProvider() noexcept = default;

  template <detail::CheckedUnsignedIntegerInput Value>
    requires(sizeof(detail::UnqualifiedIntegerInput<Value>) <= sizeof(std::uint64_t))
  explicit DeterministicClockProvider(Value initial_nanoseconds) noexcept
      : current_nanoseconds_{static_cast<std::uint64_t>(initial_nanoseconds)} {}

  template <detail::CheckedIntegerInput Value>
  [[nodiscard]] Result<void> advance(Value nanoseconds) {
    if (!std::in_range<std::uint64_t>(nanoseconds)) {
      return Result<void>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "clock_nanoseconds"));
    }
    return advance_validated(static_cast<std::uint64_t>(nanoseconds));
  }

private:
  [[nodiscard]] Result<void> advance_validated(std::uint64_t nanoseconds);

  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override {
    return current_nanoseconds_;
  }

  std::uint64_t current_nanoseconds_{0U};
};

class SystemClockProvider final : public ClockProvider {
public:
  SystemClockProvider() noexcept;

private:
  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override;

  std::chrono::steady_clock::time_point origin_;
};

} // namespace aegis::model
