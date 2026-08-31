// Purpose: validate deterministic encoder scripts and produce the exact bounded AEGISFOE schema-one
// bytes from one admitted risk-approved OMS record without rounding or reinterpretation.

#include "aegis/execution/fake_order_encoder.hpp"

#include "aegis/model/sha256.hpp"
#include "aegis/oms/outbound_oms.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace aegis::execution {
namespace {

// --------------------------------------------------------
// Only the two assigned values are valid AEGISSUP encoder actions.
[[nodiscard]] bool is_assigned(FakeEncodingAction action) noexcept {
  return action == FakeEncodingAction::Encode || action == FakeEncodingAction::Fail;
}

// --------------------------------------------------------
// Name impossible local fake states without turning them into ordinary scripted EncodingFailed.
[[nodiscard]] model::DomainError create_invalid_fake_state_error(std::string field) {
  return model::DomainError::create_at_field(model::DomainErrorCode::InvalidFakeState,
                                             std::move(field));
}

// ########################################################################
// A fixed writer implements only the primitive positional AEGISFOE schema and never allocates,
// resizes, rounds, or applies host byte order.
class FakeOrderWriter final {
public:

  // --------------------------------------------------------
  // Restrict writes to the validated policy-selected prefix of the compiled 1,024-byte storage.
  explicit FakeOrderWriter(std::uint16_t capacity) noexcept : capacity_{capacity} {}

  // --------------------------------------------------------
  // Append one raw byte only when it fits the policy-selected bound.
  [[nodiscard]] bool append_u8(std::uint8_t value) noexcept {
    const std::array bytes{std::byte{value}};
    return append_bytes(bytes);
  }

  // --------------------------------------------------------
  // Append one unsigned 16-bit scalar in portable big-endian order.
  [[nodiscard]] bool append_u16(std::uint16_t value) noexcept {
    const std::array bytes{std::byte{static_cast<std::uint8_t>((value >> 8U) & 0xffU)},
                           std::byte{static_cast<std::uint8_t>(value & 0xffU)}};
    return append_bytes(bytes);
  }

  // --------------------------------------------------------
  // Append one unsigned 64-bit scalar in portable big-endian order.
  [[nodiscard]] bool append_u64(std::uint64_t value) noexcept {
    std::array<std::byte, 8U> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      const auto shift = static_cast<unsigned>((bytes.size() - 1U - index) * 8U);
      bytes[index] = std::byte{static_cast<std::uint8_t>((value >> shift) & 0xffU)};
    }
    return append_bytes(bytes);
  }

  // --------------------------------------------------------
  // Signed coefficients use their modulo-converted two's-complement bit pattern at fixed width.
  [[nodiscard]] bool append_i64(std::int64_t value) noexcept {
    return append_u64(static_cast<std::uint64_t>(value));
  }

  // --------------------------------------------------------
  // Append exact bytes after a subtraction-form capacity check that cannot wrap.
  [[nodiscard]] bool append_bytes(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() > capacity_ - size_) {
      return false;
    }
    std::copy(bytes.begin(), bytes.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(size_));
    size_ += bytes.size();
    return true;
  }

  // --------------------------------------------------------
  // Append the fixed ASCII magic without a terminator or encoding conversion.
  [[nodiscard]] bool append_ascii(std::string_view value) noexcept {
    return append_bytes(std::as_bytes(std::span<const char>{value.data(), value.size()}));
  }

  // --------------------------------------------------------
  // Prefix one already-validated identifier with its exact unsigned 16-bit byte length.
  [[nodiscard]] bool append_identifier(std::string_view value) noexcept {
    if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
      return false;
    }
    return append_u16(static_cast<std::uint16_t>(value.size())) && append_ascii(value);
  }

  // --------------------------------------------------------
  // Append one canonical nominal decimal without changing coefficient or scale.
  template <typename Decimal> [[nodiscard]] bool append_decimal(Decimal value) noexcept {
    return append_i64(value.coefficient()) && append_u8(value.scale());
  }

  // --------------------------------------------------------
  // Return the number of initialized bytes currently held by the bounded encoder.
  [[nodiscard]] std::size_t encoded_byte_count() const noexcept { return size_; }

  // --------------------------------------------------------
  // Transfer the fixed storage after the caller has captured the used prefix length.
  [[nodiscard]] std::array<std::byte, maximum_encoded_fake_order_bytes> take_bytes() && noexcept {
    return std::move(bytes_);
  }

  // --------------------------------------------------------
private:
  std::array<std::byte, maximum_encoded_fake_order_bytes> bytes_{};
  std::size_t capacity_;
  std::size_t size_{0U};
};

// ########################################################################

// --------------------------------------------------------
// Convert the trusted fixed-width order identity into a byte span without copying.
[[nodiscard]] std::span<const std::byte> order_id_bytes(const model::OrderId& order_id) noexcept {
  return std::as_bytes(std::span{order_id.bytes()});
}

// --------------------------------------------------------
// Append one raw digest without a length or wrapper-specific encoding.
[[nodiscard]] bool append_digest(FakeOrderWriter& writer,
                                 const model::Sha256Digest& digest) noexcept {
  return writer.append_bytes(digest);
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Validate actions and bounds before sorting authored overrides into their canonical order.
model::Result<FakeEncoderScript>
FakeEncoderScript::create_fake_encoder_script(FakeEncodingAction default_action,
                                              std::uint64_t maximum_invocations,
                                              std::vector<FakeEncodingOverride> overrides) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Fail closed on an unassigned default or a maximum outside the AEGISSUP schema-one bound.
  if (!is_assigned(default_action)) {
    return model::Result<FakeEncoderScript>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidSubmissionPolicy,
                                            "submission_policy.encoder_script.default_action"));
  }
  if (maximum_invocations == 0U || maximum_invocations > maximum_submission_attempts_supported) {
    return model::Result<FakeEncoderScript>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidSubmissionPolicy,
                                            "submission_policy.maximum_submission_attempts"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical order makes authored input order irrelevant to action selection and policy bytes.
  std::sort(overrides.begin(), overrides.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.invocation_ordinal < rhs.invocation_ordinal;
  });

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject every zero, out-of-range, duplicate, or unassigned override before publication.
  for (std::size_t index = 0U; index < overrides.size(); ++index) {
    const auto& override = overrides[index];
    if (override.invocation_ordinal == 0U || override.invocation_ordinal > maximum_invocations ||
        !is_assigned(override.action) ||
        (index != 0U && overrides[index - 1U].invocation_ordinal == override.invocation_ordinal)) {
      return model::Result<FakeEncoderScript>::create_failure(
          model::DomainError::create_at_index(model::DomainErrorCode::InvalidSubmissionPolicy,
                                              "submission_policy.encoder_script.overrides", index));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the validated default plus canonical unique override list as one immutable value.
  return model::Result<FakeEncoderScript>::create_success(
      FakeEncoderScript{default_action, maximum_invocations, std::move(overrides)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Retain the already-validated canonical script without another allocation or reorder.
FakeEncoderScript::FakeEncoderScript(FakeEncodingAction default_action,
                                     std::uint64_t maximum_invocations,
                                     std::vector<FakeEncodingOverride> overrides)
    : default_action_{default_action}, maximum_invocations_{maximum_invocations},
      overrides_{std::move(overrides)} {}

// --------------------------------------------------------
// Binary search the canonical unique list and otherwise return the non-exhausting default.
FakeEncodingAction
FakeEncoderScript::action_for(model::EncoderInvocationOrdinal ordinal) const noexcept {
  const auto found =
      std::lower_bound(overrides_.begin(), overrides_.end(), ordinal.value(),
                       [](const FakeEncodingOverride& override, std::uint64_t value) {
                         return override.invocation_ordinal < value;
                       });
  return found != overrides_.end() && found->invocation_ordinal == ordinal.value()
             ? found->action
             : default_action_;
}

// --------------------------------------------------------
// Own one exact byte result plus the non-payload identities needed by fake acceptance evidence.
EncodedFakeOrder::EncodedFakeOrder(model::SubmissionAttemptId attempt_id,
                                   model::EncoderInvocationOrdinal invocation_ordinal,
                                   std::array<std::byte, maximum_encoded_fake_order_bytes> bytes,
                                   std::uint16_t byte_length) noexcept
    : attempt_id_{attempt_id}, invocation_ordinal_{invocation_ordinal}, bytes_{std::move(bytes)},
      byte_length_{byte_length} {}

// --------------------------------------------------------
// A scripted failure consumes its invocation but publishes no byte object.
FakeEncodingResult::FakeEncodingResult(model::EncoderInvocationOrdinal invocation_ordinal) noexcept
    : action_{FakeEncodingAction::Fail}, invocation_ordinal_{invocation_ordinal} {}

// --------------------------------------------------------
// A successful result derives its action and invocation from the sole exact encoded object.
FakeEncodingResult::FakeEncodingResult(EncodedFakeOrder encoded_order) noexcept
    : action_{FakeEncodingAction::Encode}, invocation_ordinal_{encoded_order.invocation_ordinal()},
      encoded_order_{std::move(encoded_order)} {}

// --------------------------------------------------------
// Validate the fixed local byte bound without accepting any transport-shaped parameter.
model::Result<DeterministicFakeOrderEncoder>
DeterministicFakeOrderEncoder::create_deterministic_fake_order_encoder(
    FakeEncoderScript script, std::uint16_t byte_capacity) {
  if (byte_capacity == 0U || byte_capacity > maximum_encoded_fake_order_bytes) {
    return model::Result<DeterministicFakeOrderEncoder>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidSubmissionPolicy,
                                            "submission_policy.encoded_byte_capacity"));
  }
  return model::Result<DeterministicFakeOrderEncoder>::create_success(
      DeterministicFakeOrderEncoder{std::move(script), byte_capacity});
}

// --------------------------------------------------------
// Retain the validated script and policy-selected fixed byte ceiling for all later invocations.
DeterministicFakeOrderEncoder::DeterministicFakeOrderEncoder(FakeEncoderScript script,
                                                             std::uint16_t byte_capacity)
    : script_{std::move(script)}, byte_capacity_{byte_capacity} {}

// --------------------------------------------------------
// Consume before selection so scripted failures and successful encodes advance identically.
model::Result<model::EncoderInvocationOrdinal> DeterministicFakeOrderEncoder::consume_invocation() {
  if (invocations_consumed_ == script_.maximum_invocations()) {
    return model::Result<model::EncoderInvocationOrdinal>::create_failure(
        create_invalid_fake_state_error("fake_order_encoder.invocation_ordinal"));
  }
  ++invocations_consumed_;
  return model::EncoderInvocationOrdinal::from_value(invocations_consumed_);
}

// --------------------------------------------------------
// Apply the selected deterministic action, then encode every listed AEGISFOE field in exact order.
model::Result<FakeEncodingResult>
DeterministicFakeOrderEncoder::encode_order(const oms::OutboundOrderRecord& order) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Every reached call consumes one ordinal and its selected action before any later check.
  auto invocation = consume_invocation();
  if (!invocation) {
    return model::Result<FakeEncodingResult>::create_failure(invocation.error());
  }
  const auto action = script_.action_for(invocation.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Defensive validation rejects impossible coordinator/OMS disagreement as an internal fake fault.
  const auto& admission = order.admission();
  if (order.state() != oms::OutboundOrderState::PendingEncoding ||
      admission.attempt_id.value() != admission.reservation_id.value() ||
      admission.economics.quantity != admission.exposure.order_quantity()) {
    return model::Result<FakeEncodingResult>::create_failure(
        create_invalid_fake_state_error("fake_order_encoder.order"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A valid scripted failure is ordinary and creates no byte object or initiator work.
  if (action == FakeEncodingAction::Fail) {
    return model::Result<FakeEncodingResult>::create_success(
        FakeEncodingResult{invocation.value()});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Encode the exact listed positional schema; decimals contribute stored coefficient and scale.
  const auto& economics = admission.economics;
  const auto& provenance = admission.provenance;
  FakeOrderWriter writer{byte_capacity_};
  const bool encoded =
      writer.append_ascii("AEGISFOE") && writer.append_u16(canonical_fake_order_schema_version) &&
      writer.append_bytes(order_id_bytes(admission.order_id)) &&
      writer.append_identifier(provenance.route_id.value()) &&
      writer.append_identifier(provenance.venue_id.value()) &&
      writer.append_identifier(provenance.logical_account_id.value()) &&
      writer.append_identifier(provenance.instrument_id.value()) &&
      writer.append_identifier(provenance.venue_instrument_id.value()) &&
      writer.append_identifier(provenance.firm_id.value()) &&
      writer.append_identifier(provenance.desk_id.value()) &&
      writer.append_identifier(provenance.bot_id.value()) &&
      writer.append_identifier(provenance.strategy_id.value()) &&
      writer.append_u8(static_cast<std::uint8_t>(economics.side)) &&
      writer.append_u8(static_cast<std::uint8_t>(economics.type)) &&
      writer.append_u8(static_cast<std::uint8_t>(economics.time_in_force)) &&
      writer.append_decimal(economics.price) && writer.append_decimal(economics.quantity) &&
      append_digest(writer, provenance.configuration_fingerprint) &&
      writer.append_u64(provenance.organization_revision.value()) &&
      writer.append_u64(provenance.route_revision.value()) &&
      writer.append_u64(provenance.metadata_revision.value()) &&
      append_digest(writer, provenance.runtime_policy_fingerprint) &&
      append_digest(writer, provenance.risk_policy_fingerprint) &&
      writer.append_u64(provenance.risk_policy_revision.value()) &&
      append_digest(writer, provenance.submission_policy_fingerprint) &&
      writer.append_u64(admission.reservation_id.value());
  if (!encoded || writer.encoded_byte_count() > std::numeric_limits<std::uint16_t>::max()) {
    return model::Result<FakeEncodingResult>::create_failure(
        create_invalid_fake_state_error("fake_order_encoder.bytes"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the exact used prefix and keep attempt/invocation provenance outside canonical bytes.
  const auto byte_length = static_cast<std::uint16_t>(writer.encoded_byte_count());
  EncodedFakeOrder encoded_order{admission.attempt_id, invocation.value(),
                                 std::move(writer).take_bytes(), byte_length};
  return model::Result<FakeEncodingResult>::create_success(
      FakeEncodingResult{std::move(encoded_order)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::execution
