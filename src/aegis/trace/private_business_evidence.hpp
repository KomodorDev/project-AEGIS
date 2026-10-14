// Purpose: retain complete allocation-free M4 business evidence values and immutable proposed audit
// rows backed by cold fixed-capacity storage, without publication or callback-delivery authority.

#pragma once

#include "aegis/model/domain_error.hpp"
#include "aegis/model/m4_provenance.hpp"
#include "aegis/oms/outbound_oms.hpp"
#include "aegis/oms/private_order_resolution.hpp"
#include "aegis/recovery/journal_record.hpp"
#include "aegis/risk/account_safety.hpp"
#include "aegis/risk/reservation_ledger.hpp"
#include "aegis/trace/m4_semantic_evidence.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// The source-private preparation owner alone seals proposed records and writes their cold backing.
class PrivateBusinessEvidenceStore;

// ########################################################################

} // namespace aegis::runtime

namespace aegis::trace {

// ########################################################################
// A currency owns exactly the existing metadata grammar in inline storage: 3 through 12 uppercase
// ASCII letters or digits, beginning with a letter. Copies allocate nothing and preserve spelling.
class QuoteCurrency final {
public:

  // --------------------------------------------------------
  // Validate the complete metadata currency grammar before copying into bounded inline storage.
  [[nodiscard]] static model::Result<QuoteCurrency> parse_quote_currency(std::string_view value);

  // --------------------------------------------------------
  // Borrow the exact validated currency spelling without retaining caller-owned text.
  [[nodiscard]] std::string_view value() const noexcept { return {bytes_.data(), size_}; }

  // --------------------------------------------------------
  // Compare complete semantic currency spelling, including its length.
  friend bool operator==(const QuoteCurrency&, const QuoteCurrency&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Retain one already grammar-checked currency and a zeroed unused tail.
  explicit QuoteCurrency(std::string_view value) noexcept;

  // --------------------------------------------------------
  std::array<char, 12U> bytes_{};
  std::uint8_t size_;
};

// ########################################################################
// Complete error evidence owns its code, exact field spelling and optional position. Inline
// capacity is a semantic storage bound, not an ADR-0014 encoding width; copies allocate nothing.
class BoundedAuditDomainError final {
public:

  // --------------------------------------------------------
  // Reject field text longer than 256 bytes rather than truncating any machine-readable context.
  [[nodiscard]] static model::Result<BoundedAuditDomainError>
  from_domain_error(const model::DomainError& error);

  // --------------------------------------------------------
  // Return the original stable domain error assignment.
  [[nodiscard]] model::DomainErrorCode code() const noexcept { return code_; }

  // --------------------------------------------------------
  // Borrow every original field byte, including embedded zero bytes when present.
  [[nodiscard]] std::string_view field() const noexcept { return {field_.data(), field_size_}; }

  // --------------------------------------------------------
  // Return the original optional collection position without sentinel conversion.
  [[nodiscard]] const std::optional<std::size_t>& collection_index() const noexcept {
    return collection_index_;
  }

  // --------------------------------------------------------
  // Compare all retained semantic error context and its exact presence state.
  friend bool operator==(const BoundedAuditDomainError&, const BoundedAuditDomainError&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Copy only an already bounded complete error without allocating a string.
  explicit BoundedAuditDomainError(const model::DomainError& error) noexcept;

  // --------------------------------------------------------
  model::DomainErrorCode code_;
  std::array<char, 256U> field_{};
  std::uint16_t field_size_;
  std::optional<std::size_t> collection_index_;
};

// ########################################################################
// Each active nominal identifier fixes its own risk-scope kind; an incompatible kind and subject
// cannot be authored as separate fields. Every alternative owns bounded allocation-free bytes.
using RiskScopeAuditSubject =
    std::variant<model::BotId, model::DeskId, model::FirmId, model::LogicalAccountId,
                 model::RouteId, model::InstrumentId, model::VenueId>;

// ########################################################################
// One inventory effect retains the complete canonical scope key and coherent signed confirmed
// transfer beside residual/worst-case before and after projections; it grants no cell authority.
struct InventoryScopeAuditEffect {
  model::FirmId firm_id;
  RiskScopeAuditSubject subject;
  model::InstrumentId instrument_id;
  QuoteCurrency quote_currency;
  risk::RiskScopeExposure before;
  risk::RiskScopeExposure after;
  model::Quantity signed_quantity_delta;
  model::Notional signed_notional_delta;

  // --------------------------------------------------------
  // Compare the complete key and every numeric projection without storage-address dependence.
  friend bool operator==(const InventoryScopeAuditEffect&,
                         const InventoryScopeAuditEffect&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// A proposed account transition reports its exact correctness-state change and retained reason;
// it does not itself clear safety or update a submission gate.
struct AccountSafetyAuditTransition {
  risk::AccountSafetyState before;
  risk::AccountSafetyState after;
  risk::AccountSafetyReason reason;

  // --------------------------------------------------------
  // Compare the complete proposed safety transition.
  friend bool operator==(const AccountSafetyAuditTransition&,
                         const AccountSafetyAuditTransition&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One known-order primary effect owns exact local attribution, OMS and reservation projections,
// optional seven-scope economics, and optional safety change. These values describe a proposal;
// neither construction nor inspection proves that its state changes occurred.
struct PrivateOrderAuditEffect {
  model::OrderId order_id;
  model::M4Provenance provenance;
  oms::PrivateOrderProjection oms_before;
  oms::PrivateOrderProjection oms_after;
  risk::ReservationEvidence reservation_before;
  risk::ReservationEvidence reservation_after;
  std::optional<std::array<InventoryScopeAuditEffect, 7U>> inventory_effects;
  std::optional<AccountSafetyAuditTransition> account_safety_transition;

  // --------------------------------------------------------
  // Compare all copied before/after evidence without claiming application authority.
  friend bool operator==(const PrivateOrderAuditEffect&, const PrivateOrderAuditEffect&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One proposed order callback owns the exact local subject and originating source identity. An
// execution names its trade; an applied cumulative endpoint is present only when economics apply.
struct OrderCallbackAuditValue {
  model::OrderId order_id;
  model::M4Provenance provenance;
  OriginatingEventIdentity originating_event;
  std::optional<oms::TradeId> trade_id;
  std::optional<model::Quantity> applied_cumulative_quantity;

  // --------------------------------------------------------
  // Compare every prospective notification fact without proving callback entry or delivery.
  friend bool operator==(const OrderCallbackAuditValue&, const OrderCallbackAuditValue&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// A closed immutable event observation owns all source facts, equality projections, and proposed
// linkage. Only the preparation store constructs it; copied values never grant admission,
// publication, economic consumption, or durability authority.
class PrivateEventEvidence final {
public:

  // --------------------------------------------------------
  // Borrow the original complete normalized input without rewriting its source provenance.
  [[nodiscard]] const oms::NormalizedPrivateOrderInput& input() const noexcept { return input_; }

  // --------------------------------------------------------
  // Borrow the correlation-independent, receive-time-free event comparison value.
  [[nodiscard]] const oms::PrivateEventIngressSemanticValue&
  ingress_semantic_value() const noexcept {
    return ingress_semantic_value_;
  }

  // --------------------------------------------------------
  // Borrow the sealed immutable first-admission resolution rather than rerunning correlation.
  [[nodiscard]] const oms::PrivateEventResolution& first_admission_resolution() const noexcept {
    return resolution_;
  }

  // --------------------------------------------------------
  // Borrow the post-correlation trade comparison value only when trade classification was reached.
  [[nodiscard]] const std::optional<oms::PrivateTradeSemanticValue>&
  trade_semantic_value() const noexcept {
    return trade_semantic_value_;
  }

  // --------------------------------------------------------
  // Return the runtime incarnation qualifying all process-local observation ordinals.
  [[nodiscard]] const recovery::RuntimeEpochId& runtime_epoch_id() const noexcept {
    return runtime_epoch_id_;
  }

  // --------------------------------------------------------
  // Borrow the exact owner root independently from the unchanged normalized source root.
  [[nodiscard]] const model::M4RootProvenance& root_provenance() const noexcept {
    return root_provenance_;
  }

  // --------------------------------------------------------
  // Borrow only the normalized source subject; known local resolution is retained separately.
  [[nodiscard]] const std::optional<model::M4SubjectProvenance>&
  subject_provenance() const noexcept {
    return input_.provenance().subject();
  }

  // --------------------------------------------------------
  // Return both executor identities without adding them to duplicate-comparison semantics.
  [[nodiscard]] const recovery::JournalReplayProvenance& replay_provenance() const noexcept {
    return replay_provenance_;
  }

  // --------------------------------------------------------
  // Return the observation's global attempt identity.
  [[nodiscard]] model::AdmissionOrdinal admission_ordinal() const noexcept {
    return replay_provenance_.admission_ordinal;
  }

  // --------------------------------------------------------
  // Return the accepted copy's global receive identity.
  [[nodiscard]] model::ReceiveSequence receive_sequence() const noexcept {
    return replay_provenance_.receive_sequence;
  }

  // --------------------------------------------------------
  // Return the original trusted receive timestamp from the complete normalized input.
  [[nodiscard]] model::ReceiveTimestamp receive_time() const noexcept {
    return input_.receive_time();
  }

  // --------------------------------------------------------
  // Return the proposed disposition without proving canonical consumption.
  [[nodiscard]] oms::PrivateEventDisposition disposition() const noexcept { return disposition_; }

  // --------------------------------------------------------
  // Return the exact source-domain tag retained by the input.
  [[nodiscard]] oms::PrivateEventOrigin origin() const noexcept { return input_.origin(); }

  // --------------------------------------------------------
  // Derive the tagged originating identity from the complete original source envelope.
  [[nodiscard]] OriginatingEventIdentity originating_event() const noexcept;

  // --------------------------------------------------------
  // Return the reconciliation epoch only when the original input has reconciliation origin.
  [[nodiscard]] std::optional<recovery::ReconciliationEpochId>
  reconciliation_epoch_id() const noexcept;

  // --------------------------------------------------------
  // Borrow optional prospective journal linkage; absence does not manufacture a sequence.
  [[nodiscard]] const std::optional<recovery::JournalSequence>& journal_sequence() const noexcept {
    return journal_sequence_;
  }

  // --------------------------------------------------------
  // Borrow the exact optional diagnostic cross-reference, independently of any audit span.
  [[nodiscard]] const std::optional<recovery::DiagnosticOrdinal>&
  diagnostic_ordinal() const noexcept {
    return diagnostic_ordinal_;
  }

  // --------------------------------------------------------
  // Compare every owned semantic field, including source receive time and optional linkage.
  friend bool operator==(const PrivateEventEvidence&, const PrivateEventEvidence&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Seal an already validated complete event proposal without performing hot allocation.
  PrivateEventEvidence(oms::NormalizedPrivateOrderInput input,
                       oms::PrivateEventIngressSemanticValue ingress_semantic_value,
                       oms::PrivateEventResolution resolution,
                       std::optional<oms::PrivateTradeSemanticValue> trade_semantic_value,
                       recovery::RuntimeEpochId runtime_epoch_id,
                       model::M4RootProvenance root_provenance,
                       recovery::JournalReplayProvenance replay_provenance,
                       oms::PrivateEventDisposition disposition,
                       std::optional<recovery::JournalSequence> journal_sequence,
                       std::optional<recovery::DiagnosticOrdinal> diagnostic_ordinal) noexcept;

  // --------------------------------------------------------
  oms::NormalizedPrivateOrderInput input_;
  oms::PrivateEventIngressSemanticValue ingress_semantic_value_;
  oms::PrivateEventResolution resolution_;
  std::optional<oms::PrivateTradeSemanticValue> trade_semantic_value_;
  recovery::RuntimeEpochId runtime_epoch_id_;
  model::M4RootProvenance root_provenance_;
  recovery::JournalReplayProvenance replay_provenance_;
  oms::PrivateEventDisposition disposition_;
  std::optional<recovery::JournalSequence> journal_sequence_;
  std::optional<recovery::DiagnosticOrdinal> diagnostic_ordinal_;

  // ########################################################################
  // Only the policy-sized source-private store constructs complete proposed evidence.
  friend class runtime::PrivateBusinessEvidenceStore;

  // ########################################################################
};

// ########################################################################
// A primary buffer is sized exactly once during cold construction. Shared const record views keep
// retained effects alive after the owner dies; only the store may populate empty optional slots.
class PrivateOrderAuditEffectBuffer final {
public:

  // --------------------------------------------------------
  // Preserve one stable bounded backing rather than duplicating or relocating retained storage.
  PrivateOrderAuditEffectBuffer(const PrivateOrderAuditEffectBuffer&) = delete;
  PrivateOrderAuditEffectBuffer& operator=(const PrivateOrderAuditEffectBuffer&) = delete;
  PrivateOrderAuditEffectBuffer(PrivateOrderAuditEffectBuffer&&) = delete;
  PrivateOrderAuditEffectBuffer& operator=(PrivateOrderAuditEffectBuffer&&) = delete;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Allocate the complete policy-selected capacity before any proposed owner turn exists.
  explicit PrivateOrderAuditEffectBuffer(std::size_t capacity) : values_(capacity) {}

  // --------------------------------------------------------
  std::vector<std::optional<PrivateOrderAuditEffect>> values_;

  // ########################################################################
  // The store owns population; immutable audit rows own inspection only.
  friend class runtime::PrivateBusinessEvidenceStore;
  friend class OrderAuditRecord;

  // ########################################################################
};

// ########################################################################
// A callback buffer is cold sized once and leased immutably by proposed Planned or terminal rows.
// Shared ownership extends copied notification lifetime without transferring callback authority.
class OrderCallbackAuditBuffer final {
public:

  // --------------------------------------------------------
  // Preserve one stable bounded callback backing throughout every outstanding proposed row.
  OrderCallbackAuditBuffer(const OrderCallbackAuditBuffer&) = delete;
  OrderCallbackAuditBuffer& operator=(const OrderCallbackAuditBuffer&) = delete;
  OrderCallbackAuditBuffer(OrderCallbackAuditBuffer&&) = delete;
  OrderCallbackAuditBuffer& operator=(OrderCallbackAuditBuffer&&) = delete;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Allocate all callback values before any preparation may borrow the buffer.
  explicit OrderCallbackAuditBuffer(std::size_t capacity) : values_(capacity) {}

  // --------------------------------------------------------
  std::vector<std::optional<OrderCallbackAuditValue>> values_;

  // ########################################################################
  // The store owns population; immutable audit rows own inspection only.
  friend class runtime::PrivateBusinessEvidenceStore;
  friend class OrderAuditRecord;

  // ########################################################################
};

// ########################################################################
// A closed move-only audit row owns fixed header values and shares immutable cold effect/callback
// backing. The prepared capability determines whether this is a proposal; this type alone proves
// neither applied business state nor callback delivery. Views survive owner destruction; the sole
// prepared lease prevents backing reuse until every proposed row in that capability is destroyed.
class OrderAuditRecord final {
public:

  // --------------------------------------------------------
  // Interesting syntax: move-only rows transfer buffer leases; const prepared getters cannot be
  // moved into an append operation or duplicate publication authority.
  OrderAuditRecord(const OrderAuditRecord&) = delete;
  OrderAuditRecord& operator=(const OrderAuditRecord&) = delete;
  OrderAuditRecord(OrderAuditRecord&&) noexcept = default;
  OrderAuditRecord& operator=(OrderAuditRecord&&) noexcept = default;

  // --------------------------------------------------------
  // Return the runtime incarnation that qualifies the row's audit ordinal.
  [[nodiscard]] const recovery::RuntimeEpochId& runtime_epoch_id() const noexcept {
    return runtime_epoch_id_;
  }

  // --------------------------------------------------------
  // Return the row's preassigned position within its proposed contiguous span.
  [[nodiscard]] recovery::AuditOrdinal audit_ordinal() const noexcept { return audit_ordinal_; }

  // --------------------------------------------------------
  // Return the stable primary or aggregate callback profile selected by the store.
  [[nodiscard]] M4AuditKind kind() const noexcept { return kind_; }

  // --------------------------------------------------------
  // Borrow the exact tagged source identity rather than a synthetic combined execution.
  [[nodiscard]] const OriginatingEventIdentity& originating_event() const noexcept {
    return originating_event_;
  }

  // --------------------------------------------------------
  // Borrow the unchanged complete source input on primary rows; aggregate callback rows omit it.
  [[nodiscard]] const std::optional<oms::NormalizedPrivateOrderInput>&
  source_input() const noexcept {
    return source_input_;
  }

  // --------------------------------------------------------
  // Borrow the complete root authorizing this store incarnation.
  [[nodiscard]] const model::M4RootProvenance& root_provenance() const noexcept {
    return root_provenance_;
  }

  // --------------------------------------------------------
  // Borrow the applicable primary/callback subject without inventing runtime-global attribution.
  [[nodiscard]] const std::optional<model::M4SubjectProvenance>&
  subject_provenance() const noexcept {
    return subject_provenance_;
  }

  // --------------------------------------------------------
  // Borrow optional prospective journal linkage without claiming input publication.
  [[nodiscard]] const std::optional<recovery::JournalSequence>& journal_sequence() const noexcept {
    return journal_sequence_;
  }

  // --------------------------------------------------------
  // Primary rows carry their proposed input disposition; callback rows omit it.
  [[nodiscard]] const std::optional<oms::PrivateEventDisposition>& disposition() const noexcept {
    return disposition_;
  }

  // --------------------------------------------------------
  // Return primary suppression or aggregate callback planning/completion vocabulary.
  [[nodiscard]] CallbackDecision callback_decision() const noexcept { return callback_decision_; }

  // --------------------------------------------------------
  // Borrow the nonempty callback range only for an aggregate callback row.
  [[nodiscard]] const std::optional<CallbackOrdinalRange>& callback_range() const noexcept {
    return callback_range_;
  }

  // --------------------------------------------------------
  // Borrow complete bounded optional fault or rejection context.
  [[nodiscard]] const std::optional<BoundedAuditDomainError>& error() const noexcept {
    return error_;
  }

  // --------------------------------------------------------
  // Return the immutable primary effect prefix; a moved-from or callback row returns zero.
  [[nodiscard]] std::uint32_t effect_count() const noexcept {
    return effects_ ? effect_count_ : 0U;
  }

  // --------------------------------------------------------
  // Borrow one owned primary effect, or return null outside the immutable populated prefix.
  [[nodiscard]] const PrivateOrderAuditEffect* effect_at(std::uint32_t index) const noexcept;

  // --------------------------------------------------------
  // Return the immutable notification prefix; a moved-from or primary row returns zero.
  [[nodiscard]] std::uint32_t callback_count() const noexcept {
    return callbacks_ ? callback_count_ : 0U;
  }

  // --------------------------------------------------------
  // Borrow one proposed notification, or return null outside the immutable populated prefix.
  [[nodiscard]] const OrderCallbackAuditValue* callback_at(std::uint32_t index) const noexcept;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Seal a completely validated primary or aggregate callback profile without allocating.
  OrderAuditRecord(recovery::RuntimeEpochId runtime_epoch_id, recovery::AuditOrdinal audit_ordinal,
                   M4AuditKind kind, OriginatingEventIdentity originating_event,
                   std::optional<oms::NormalizedPrivateOrderInput> source_input,
                   model::M4RootProvenance root_provenance,
                   std::optional<model::M4SubjectProvenance> subject_provenance,
                   std::optional<recovery::JournalSequence> journal_sequence,
                   std::optional<oms::PrivateEventDisposition> disposition,
                   CallbackDecision callback_decision,
                   std::optional<CallbackOrdinalRange> callback_range,
                   std::optional<BoundedAuditDomainError> error,
                   std::shared_ptr<const PrivateOrderAuditEffectBuffer> effects,
                   std::uint32_t effect_count,
                   std::shared_ptr<const OrderCallbackAuditBuffer> callbacks,
                   std::uint32_t callback_count) noexcept;

  // --------------------------------------------------------
  recovery::RuntimeEpochId runtime_epoch_id_;
  recovery::AuditOrdinal audit_ordinal_;
  M4AuditKind kind_;
  OriginatingEventIdentity originating_event_;
  std::optional<oms::NormalizedPrivateOrderInput> source_input_;
  model::M4RootProvenance root_provenance_;
  std::optional<model::M4SubjectProvenance> subject_provenance_;
  std::optional<recovery::JournalSequence> journal_sequence_;
  std::optional<oms::PrivateEventDisposition> disposition_;
  CallbackDecision callback_decision_;
  std::optional<CallbackOrdinalRange> callback_range_;
  std::optional<BoundedAuditDomainError> error_;
  std::shared_ptr<const PrivateOrderAuditEffectBuffer> effects_;
  std::uint32_t effect_count_;
  std::shared_ptr<const OrderCallbackAuditBuffer> callbacks_;
  std::uint32_t callback_count_;

  // ########################################################################
  // Only the source-private preparation store can bind a row to validated cold backing.
  friend class runtime::PrivateBusinessEvidenceStore;

  // ########################################################################
};

// ########################################################################

// --------------------------------------------------------
// Return the risk-scope assignment implied by the active nominal subject alternative.
[[nodiscard]] risk::RiskScopeKind risk_scope_kind(const RiskScopeAuditSubject& subject) noexcept;

// --------------------------------------------------------
// Borrow the complete nominal subject spelling for explicit canonical key comparison.
[[nodiscard]] std::string_view
risk_scope_subject_value(const RiskScopeAuditSubject& subject) noexcept;

// --------------------------------------------------------
// Compare complete inventory-effect keys in firm, scope, subject, instrument, currency order.
[[nodiscard]] bool
is_inventory_scope_audit_effect_less(const InventoryScopeAuditEffect& left,
                                     const InventoryScopeAuditEffect& right) noexcept;

// --------------------------------------------------------
// Derive the closed source identity while preserving the original local, venue, or cut-row domain.
[[nodiscard]] OriginatingEventIdentity originating_event_identity_from_normalized_input(
    const oms::NormalizedPrivateOrderInput& input) noexcept;

// --------------------------------------------------------
// Validate one proposed known-order effect's complete subject, state, reservation, seven-scope
// ordering, and signed confirmed arithmetic before the preparation store retains any values.
[[nodiscard]] model::Result<void>
validate_private_order_audit_effect(const PrivateOrderAuditEffect& effect,
                                    const model::M4RootProvenance& owner_root);

// --------------------------------------------------------

} // namespace aegis::trace
