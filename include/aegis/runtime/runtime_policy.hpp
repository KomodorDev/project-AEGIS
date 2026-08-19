// Purpose: define the immutable capacity, freshness, and configured-source contract for M2 replay.

#pragma once

#include "aegis/configuration/configuration_provenance.hpp"
#include "aegis/market_data/market_limits.hpp"
#include "aegis/market_data/subscription.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aegis::configuration {

// ########################################################################
// The startup snapshot remains the authority for venues, instruments, and observation grants.
class StartupConfiguration;

// ########################################################################

} // namespace aegis::configuration

namespace aegis::runtime {

// ########################################################################
// The immutable policy is the only publisher of validated configured-source identities.
class RuntimePolicy;

// ########################################################################
// The schema version belongs only to AEGISRTP bytes and does not alter AEGISCFG schema one.
inline constexpr std::uint16_t canonical_runtime_policy_schema_version = 1U;

// Compatibility aliases keep policy validation readable while deriving from the one shared market
// contract rather than repeating parser, update, and book constants.
inline constexpr std::uint32_t maximum_runtime_frame_bytes =
    static_cast<std::uint32_t>(market_data::maximum_recorded_frame_bytes);
inline constexpr std::uint32_t maximum_runtime_changes_per_update =
    static_cast<std::uint32_t>(market_data::maximum_changes_per_market_update);
inline constexpr std::uint32_t maximum_runtime_retained_book_depth =
    static_cast<std::uint32_t>(market_data::maximum_retained_book_depth);

// ########################################################################

// ########################################################################
// Runtime limits are authored together and become immutable only after RuntimePolicy::create has
// established positivity and the M2 compile-time ceilings.
struct RuntimePolicyLimits {
  std::uint32_t ingress_capacity;
  std::uint32_t maximum_frame_bytes;
  std::uint32_t maximum_changes_per_update;
  std::uint32_t retained_book_depth;
  std::uint64_t stale_threshold_nanoseconds;
  std::uint32_t maximum_callbacks_per_turn;
  std::uint32_t diagnostic_capacity;
  std::uint32_t runtime_trace_capacity;
  std::uint32_t maximum_drive_turns;
  std::uint64_t callback_budget_nanoseconds;

  // --------------------------------------------------------
  // Structural equality compares every scheduling, storage, freshness, and measurement bound.
  friend bool operator==(const RuntimePolicyLimits&, const RuntimePolicyLimits&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Authored source definitions contain only normalized public-market identity and the exact
// metadata revision; M2 fixes the channel to OrderBook during validation and encoding.
struct RuntimeSourceDefinition {
  model::MarketSourceId source_id;
  model::VenueId venue_id;
  model::InstrumentId instrument_id;
  model::VenueInstrumentId venue_instrument_id;
  model::InstrumentMetadataRevision metadata_revision;

  // --------------------------------------------------------
  // Structural equality compares the complete authored source-to-metadata relationship.
  friend bool operator==(const RuntimeSourceDefinition&, const RuntimeSourceDefinition&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// This key is the M2 uniqueness and subscription-dispatch identity. A later decision is required
// before redundant sources may share one key.
struct RuntimeSourceKey {
  model::VenueId venue_id;
  model::InstrumentId instrument_id;
  market_data::SubscriptionChannel channel;

  // --------------------------------------------------------
  // Structural equality compares one complete normalized observation key.
  friend bool operator==(const RuntimeSourceKey&, const RuntimeSourceKey&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Published sources pair a validated definition with its stable one-based canonical-list ordinal.
class RuntimeSource final {
public:

  // --------------------------------------------------------
  // Borrow the complete definition that was validated against the sealed startup configuration.
  [[nodiscard]] const RuntimeSourceDefinition& definition() const noexcept { return definition_; }

  // --------------------------------------------------------
  // Return the stable one-based position assigned in canonical source-ID order.
  [[nodiscard]] model::MarketSourceOrdinal ordinal() const noexcept { return ordinal_; }

  // --------------------------------------------------------
  // Return the exact order-book grant count derived from the sealed startup configuration.
  [[nodiscard]] std::uint32_t matching_subscription_count() const noexcept {
    return matching_subscription_count_;
  }

  // --------------------------------------------------------
  // Return the fixed M2 observation channel without storing a caller-selectable value.
  [[nodiscard]] static constexpr market_data::SubscriptionChannel channel() noexcept {
    return market_data::SubscriptionChannel::OrderBook;
  }

  // --------------------------------------------------------
  // Structural equality compares both validated identity and assigned canonical ordinal.
  friend bool operator==(const RuntimeSource&, const RuntimeSource&) = default;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only complete RuntimePolicy validation may mint a configured source identity.
  friend class RuntimePolicy;

  // ########################################################################

  // --------------------------------------------------------
  // Pair one validated definition with the ordinal derived from final canonical order.
  RuntimeSource(RuntimeSourceDefinition definition, model::MarketSourceOrdinal ordinal,
                std::uint32_t matching_subscription_count)
      : definition_{std::move(definition)}, ordinal_{ordinal},
        matching_subscription_count_{matching_subscription_count} {}

  // --------------------------------------------------------
  RuntimeSourceDefinition definition_;
  model::MarketSourceOrdinal ordinal_;
  std::uint32_t matching_subscription_count_;
};

// ########################################################################

// ########################################################################
// Authoring parameters remain mutable outside the published value so failed creation cannot expose
// a partially sorted or partially validated policy.
struct RuntimePolicyParams {
  RuntimePolicyLimits limits;
  std::vector<RuntimeSourceDefinition> sources;
};

// ########################################################################

// ########################################################################
// This digest names exact schema-versioned runtime-policy bytes. Hexadecimal output is only a
// display representation of the same fixed-width SHA-256 identity.
class RuntimePolicyFingerprint final {
public:

  // --------------------------------------------------------
  // Own one already-computed digest while keeping construction explicit at encoding boundaries.
  explicit RuntimePolicyFingerprint(model::Sha256Digest bytes) noexcept
      : bytes_{std::move(bytes)} {}

  // --------------------------------------------------------
  // Borrow the fixed-width binary runtime-policy identity.
  [[nodiscard]] const model::Sha256Digest& bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Render the same identity as exactly 64 lowercase hexadecimal characters.
  [[nodiscard]] std::string to_hex() const;

  // --------------------------------------------------------
  // Structural equality compares the complete digest.
  friend bool operator==(const RuntimePolicyFingerprint&,
                         const RuntimePolicyFingerprint&) = default;

  // --------------------------------------------------------
private:
  model::Sha256Digest bytes_;
};

// ########################################################################

// ########################################################################
// Successful creation publishes one immutable policy whose startup identity, limits, source
// registry, canonical bytes, and fingerprint were derived from the same validated snapshot.
class RuntimePolicy final {
public:

  // --------------------------------------------------------
  // Validate, canonicalize, encode, and fingerprint one complete M2 policy atomically.
  [[nodiscard]] static model::Result<RuntimePolicy>
  create(const configuration::StartupConfiguration& configuration, RuntimePolicyParams params);

  // --------------------------------------------------------
  // Borrow the exact sealed M1 configuration identity referenced by this policy.
  [[nodiscard]] const configuration::ConfigurationFingerprint&
  configuration_fingerprint() const noexcept {
    return configuration_fingerprint_;
  }

  // --------------------------------------------------------
  // Borrow all validated runtime bounds as one coherent value.
  [[nodiscard]] const RuntimePolicyLimits& limits() const noexcept { return limits_; }

  // --------------------------------------------------------
  // Borrow sources in canonical source-ID order with stable one-based ordinals.
  [[nodiscard]] const std::vector<RuntimeSource>& sources() const noexcept { return sources_; }

  // --------------------------------------------------------
  // Return the source registry size used to preallocate source-specific owner state and fences.
  [[nodiscard]] std::size_t source_capacity() const noexcept { return sources_.size(); }

  // --------------------------------------------------------
  // Resolve a configured source by its nominal source identity.
  [[nodiscard]] const RuntimeSource*
  find_source(const model::MarketSourceId& source_id) const noexcept;

  // --------------------------------------------------------
  // Resolve the sole configured source for one M2 venue/instrument/channel key.
  [[nodiscard]] const RuntimeSource* find_source(const RuntimeSourceKey& key) const noexcept;

  // --------------------------------------------------------
  // Borrow the schema-versioned canonical bytes from which the fingerprint was computed.
  [[nodiscard]] const std::vector<std::byte>& canonical_bytes() const noexcept {
    return canonical_bytes_;
  }

  // --------------------------------------------------------
  // Borrow the SHA-256 identity of the complete canonical runtime policy.
  [[nodiscard]] const RuntimePolicyFingerprint& fingerprint() const noexcept {
    return fingerprint_;
  }

  // --------------------------------------------------------
  // Structural equality compares the complete published policy and its derived identities.
  friend bool operator==(const RuntimePolicy&, const RuntimePolicy&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Restrict publication to the factory after every validation and derivation has succeeded.
  RuntimePolicy(configuration::ConfigurationFingerprint configuration_fingerprint,
                RuntimePolicyLimits limits, std::vector<RuntimeSource> sources,
                std::vector<std::byte> canonical_bytes, RuntimePolicyFingerprint fingerprint)
      : configuration_fingerprint_{std::move(configuration_fingerprint)}, limits_{limits},
        sources_{std::move(sources)}, canonical_bytes_{std::move(canonical_bytes)},
        fingerprint_{std::move(fingerprint)} {}

  // --------------------------------------------------------
  configuration::ConfigurationFingerprint configuration_fingerprint_;
  RuntimePolicyLimits limits_;
  std::vector<RuntimeSource> sources_;
  std::vector<std::byte> canonical_bytes_;
  RuntimePolicyFingerprint fingerprint_;
};

// ########################################################################

} // namespace aegis::runtime
