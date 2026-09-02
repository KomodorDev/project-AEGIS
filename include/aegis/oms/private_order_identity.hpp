// Purpose: define M4 OMS-owned private event, exchange order, trade, source, cut, local event, and
// cancel-attempt identities without venue-native decoding or authenticated account mapping.

#pragma once

#include "aegis/model/bounded_identity.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/recovery/recovery_identity.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::oms {
namespace detail {

// ########################################################################
// Expand only component-owned tags that share the accepted opaque private-identity error profile.
#define AEGIS_PRIVATE_OPAQUE_ID_TAG(TagName, FieldName)                                            \
  struct TagName {                                                                                 \
    static constexpr std::string_view field = FieldName;                                           \
    static constexpr model::DomainErrorCode invalid_code =                                         \
        model::DomainErrorCode::InvalidPrivateIdentity;                                            \
  }

// ########################################################################
// The exchange-order tag prevents authoritative order bytes from becoming a trade or event ID.
AEGIS_PRIVATE_OPAQUE_ID_TAG(ExchangeOrderIdTag, "exchange_order_id");

// ########################################################################
// The trade tag gives execution deduplication its own nominal identity domain.
AEGIS_PRIVATE_OPAQUE_ID_TAG(TradeIdTag, "trade_id");

// ########################################################################
// The private-event tag scopes source message identity independently of trade identity.
AEGIS_PRIVATE_OPAQUE_ID_TAG(PrivateEventIdTag, "private_event_id");

// ########################################################################
// The source-epoch tag distinguishes source generations without changing trade identity.
AEGIS_PRIVATE_OPAQUE_ID_TAG(PrivateSourceEpochIdTag, "private_source_epoch_id");

// ########################################################################
// The authoritative-cut tag is opaque because AEGIS must never order venue-native cuts.
AEGIS_PRIVATE_OPAQUE_ID_TAG(AuthoritativeCutIdTag, "authoritative_cut_id");

// Close the local macro so no downstream header can manufacture an unreviewed tag silently.
#undef AEGIS_PRIVATE_OPAQUE_ID_TAG

// ########################################################################
// Local event IDs use AEGIS-owned namespace/counter issuance and private-domain errors.
struct LocalOrderEventIdTag {
  static constexpr std::string_view field = "local_order_event_id";
  static constexpr model::DomainErrorCode invalid_code =
      model::DomainErrorCode::InvalidPrivateIdentity;
  static constexpr model::DomainErrorCode exhaustion_code =
      model::DomainErrorCode::PrivateCounterExhausted;
};

// ########################################################################

} // namespace detail

// ########################################################################
// These public OMS aliases apply nominal private meaning to neutral bounded storage mechanics.
using ExchangeOrderId = model::BoundedOpaqueIdentity<detail::ExchangeOrderIdTag>;
using TradeId = model::BoundedOpaqueIdentity<detail::TradeIdTag>;
using PrivateEventId = model::BoundedOpaqueIdentity<detail::PrivateEventIdTag>;
using PrivateSourceEpochId = model::BoundedOpaqueIdentity<detail::PrivateSourceEpochIdTag>;
using AuthoritativeCutId = model::BoundedOpaqueIdentity<detail::AuthoritativeCutIdTag>;
using LocalOrderEventId = model::NamespaceCounterIdentity<detail::LocalOrderEventIdTag>;
using LocalOrderEventIdProvider = model::NamespaceCounterIdentityProvider<LocalOrderEventId>;

// ########################################################################
// The exact venue event key survives replay and scopes ordinary event identity by source epoch.
struct VenuePrivateEventKey {
  model::VenueId venue_id;
  model::LogicalAccountId logical_account_id;
  PrivateSourceEpochId source_epoch_id;
  PrivateEventId event_id;

  // --------------------------------------------------------
  // Compare the complete source-scoped event key in declared semantic order.
  friend bool operator==(const VenuePrivateEventKey&, const VenuePrivateEventKey&) = default;
  friend auto operator<=>(const VenuePrivateEventKey&, const VenuePrivateEventKey&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Trade identity deliberately excludes private-source epoch so reconnect replay cannot refill.
struct TradeKey {
  model::VenueId venue_id;
  model::LogicalAccountId logical_account_id;
  TradeId trade_id;

  // --------------------------------------------------------
  // Compare the account-scoped authoritative trade identity without source epoch.
  friend bool operator==(const TradeKey&, const TradeKey&) = default;
  friend auto operator<=>(const TradeKey&, const TradeKey&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Exchange-order mappings remain account/venue scoped and never imply local ownership by bytes.
struct ExchangeOrderKey {
  model::VenueId venue_id;
  model::LogicalAccountId logical_account_id;
  ExchangeOrderId exchange_order_id;

  // --------------------------------------------------------
  // Compare every mapping scope before opaque exchange-order bytes.
  friend bool operator==(const ExchangeOrderKey&, const ExchangeOrderKey&) = default;
  friend auto operator<=>(const ExchangeOrderKey&, const ExchangeOrderKey&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Cancel identity includes the durability-fenced runtime epoch, complete local order, and one
// per-epoch/per-order ordinal, so a lost pre-crash attempt cannot collide after restart.
class CancelAttemptId final {
public:
  static constexpr std::size_t byte_size =
      recovery::RuntimeEpochId::byte_size + model::OrderId::byte_size + sizeof(std::uint64_t);
  using Bytes = std::array<std::uint8_t, byte_size>;
  static constexpr std::string_view field = "cancel_attempt_id";
  static constexpr model::DomainErrorCode exhaustion_code =
      model::DomainErrorCode::PrivateCounterExhausted;

  // --------------------------------------------------------
  // Validate the ordinal before composing all three identity components in fixed-width order.
  template <model::detail::CheckedIntegerInput Counter>
  [[nodiscard]] static model::Result<CancelAttemptId>
  cancel_attempt_id_from_components(const recovery::RuntimeEpochId& runtime_epoch_id,
                                    const model::OrderId& order_id, Counter counter) {
    if (!std::in_range<std::uint64_t>(counter) || counter == 0) {
      return model::Result<CancelAttemptId>::create_failure(model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidPrivateIdentity, "cancel_attempt_id"));
    }
    const auto validated_counter = static_cast<std::uint64_t>(counter);
    Bytes bytes{};
    std::copy(runtime_epoch_id.bytes().begin(), runtime_epoch_id.bytes().end(), bytes.begin());
    std::copy(order_id.bytes().begin(), order_id.bytes().end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(recovery::RuntimeEpochId::byte_size));
    for (std::size_t index = 0U; index < sizeof(validated_counter); ++index) {
      const auto shift = static_cast<unsigned int>((sizeof(validated_counter) - 1U - index) * 8U);
      bytes[recovery::RuntimeEpochId::byte_size + model::OrderId::byte_size + index] =
          static_cast<std::uint8_t>((validated_counter >> shift) & 0xffU);
    }
    return model::Result<CancelAttemptId>::create_success(
        CancelAttemptId{runtime_epoch_id, order_id, validated_counter, bytes});
  }

  // --------------------------------------------------------
  // Expose the exact 56-byte canonical projection used by evidence and recovery indexes.
  [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Retain the typed runtime epoch so recovery can distinguish restart incarnations directly.
  [[nodiscard]] constexpr const recovery::RuntimeEpochId& runtime_epoch_id() const noexcept {
    return runtime_epoch_id_;
  }

  // --------------------------------------------------------
  // Retain the complete local order identity without re-parsing the packed bytes.
  [[nodiscard]] constexpr const model::OrderId& order_id() const noexcept { return order_id_; }

  // --------------------------------------------------------
  // Expose the per-epoch/per-order raw ordinal for high-water validation.
  [[nodiscard]] constexpr std::uint64_t ordinal() const noexcept { return ordinal_; }

  // --------------------------------------------------------
  // Equality and ordering include the full typed parent identities and ordinal.
  friend constexpr bool operator==(const CancelAttemptId&, const CancelAttemptId&) = default;
  friend constexpr auto operator<=>(const CancelAttemptId&, const CancelAttemptId&) = default;

  // --------------------------------------------------------
  // Prevent construction without checked ordinal and canonical byte derivation.
private:

  // --------------------------------------------------------
  // Retain components and bytes together after the public factory has validated them.
  explicit constexpr CancelAttemptId(recovery::RuntimeEpochId runtime_epoch_id,
                                     model::OrderId order_id, std::uint64_t counter,
                                     Bytes bytes) noexcept
      : runtime_epoch_id_{runtime_epoch_id}, order_id_{order_id}, ordinal_{counter}, bytes_{bytes} {
  }

  // --------------------------------------------------------
  // Typed components remain authoritative; bytes are their deterministic projection.
  recovery::RuntimeEpochId runtime_epoch_id_;
  model::OrderId order_id_;
  std::uint64_t ordinal_;
  Bytes bytes_;
};

// ########################################################################
// One order in one runtime epoch owns a move-only monotonic cancel-attempt stream.
class CancelAttemptIdProvider final {
public:

  // --------------------------------------------------------
  // Start one order's current-runtime cancel stream at ordinal one.
  [[nodiscard]] static model::Result<CancelAttemptIdProvider>
  create_cancel_attempt_id_provider(recovery::RuntimeEpochId runtime_epoch_id,
                                    model::OrderId order_id) {
    return create_cancel_attempt_id_provider(runtime_epoch_id, order_id, 1U);
  }

  // --------------------------------------------------------
  // Restore or inject a checked first unissued ordinal without signed narrowing.
  template <model::detail::CheckedIntegerInput Counter>
  [[nodiscard]] static model::Result<CancelAttemptIdProvider>
  create_cancel_attempt_id_provider(recovery::RuntimeEpochId runtime_epoch_id,
                                    model::OrderId order_id, Counter initial_counter) {
    const auto validation = CancelAttemptId::cancel_attempt_id_from_components(
        runtime_epoch_id, order_id, initial_counter);
    if (!validation) {
      return model::Result<CancelAttemptIdProvider>::create_failure(validation.error());
    }
    return model::Result<CancelAttemptIdProvider>::create_success(CancelAttemptIdProvider{
        runtime_epoch_id, order_id, static_cast<std::uint64_t>(initial_counter)});
  }

  // --------------------------------------------------------
  // Copying is forbidden because two owners could otherwise emit the same cancel identity.
  CancelAttemptIdProvider(const CancelAttemptIdProvider&) = delete;
  CancelAttemptIdProvider& operator=(const CancelAttemptIdProvider&) = delete;

  // --------------------------------------------------------
  // Transfer the stream and make the source permanently unable to issue.
  CancelAttemptIdProvider(CancelAttemptIdProvider&& other) noexcept
      : runtime_epoch_id_{other.runtime_epoch_id_}, order_id_{other.order_id_},
        next_counter_{other.next_counter_}, exhausted_{other.exhausted_} {
    other.exhausted_ = true;
  }

  // --------------------------------------------------------
  // Replace stream ownership while poisoning the moved-from provider.
  CancelAttemptIdProvider& operator=(CancelAttemptIdProvider&& other) noexcept {
    if (this != &other) {
      runtime_epoch_id_ = other.runtime_epoch_id_;
      order_id_ = other.order_id_;
      next_counter_ = other.next_counter_;
      exhausted_ = other.exhausted_;
      other.exhausted_ = true;
    }
    return *this;
  }

  // --------------------------------------------------------
  // Emit the current ordinal exactly once, then advance or enter sticky exhaustion.
  [[nodiscard]] model::Result<CancelAttemptId> generate_next_cancel_attempt_id() {
    if (exhausted_) {
      return model::Result<CancelAttemptId>::create_failure(model::DomainError::create_at_field(
          CancelAttemptId::exhaustion_code, std::string{CancelAttemptId::field}));
    }
    auto identity = CancelAttemptId::cancel_attempt_id_from_components(runtime_epoch_id_, order_id_,
                                                                       next_counter_);
    if (next_counter_ == std::numeric_limits<std::uint64_t>::max()) {
      exhausted_ = true;
    } else {
      ++next_counter_;
    }
    return identity;
  }

  // --------------------------------------------------------
  // Only a checked factory may establish cancel-stream state.
private:

  // --------------------------------------------------------
  // Retain both parent identities and the first not-yet-issued ordinal.
  CancelAttemptIdProvider(recovery::RuntimeEpochId runtime_epoch_id, model::OrderId order_id,
                          std::uint64_t initial_counter) noexcept
      : runtime_epoch_id_{runtime_epoch_id}, order_id_{order_id}, next_counter_{initial_counter} {}

  // --------------------------------------------------------
  // The latch preserves exhaustion after UINT64_MAX was already published.
  recovery::RuntimeEpochId runtime_epoch_id_;
  model::OrderId order_id_;
  std::uint64_t next_counter_;
  bool exhausted_{false};
};

// ########################################################################

} // namespace aegis::oms
