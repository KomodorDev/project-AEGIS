// Purpose: atomically validate, canonically encode, and fingerprint the M1 startup rulebook.

#include "aegis/configuration/startup_configuration.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace aegis::configuration {
namespace {

using model::DomainError;
using model::DomainErrorCode;

// Top-level tags are part of canonical schema version 1. Nested record tags restart at one and are
// assigned explicitly in their encoding functions below.
enum class ConfigurationTag : std::uint16_t {
  ConfigurationRevision = 1,
  OrganizationRevision = 2,
  Firms = 3,
  Desks = 4,
  Bots = 5,
  StrategyConfigurationRevision = 6,
  StrategySettings = 7,
  Venues = 8,
  LogicalAccounts = 9,
  InstrumentMetadata = 10,
  SubscriptionRevision = 11,
  Subscriptions = 12,
  RouteRevision = 13,
  Routes = 14,
};

[[nodiscard]] constexpr std::uint16_t tag(ConfigurationTag value) noexcept {
  return static_cast<std::uint16_t>(value);
}

// This small writer exists only for AEGIS configuration schema 1. It deliberately is not exposed as
// a general serializer and accepts only the primitives required by ADR-0005.
class CanonicalConfigurationWriter final {
public:
  [[nodiscard]] bool append_ascii_raw(std::string_view value) {
    if (!can_grow(value.size())) {
      return false;
    }
    for (const char character : value) {
      bytes_.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return true;
  }

  [[nodiscard]] bool append_byte(std::uint8_t value) {
    if (!can_grow(1U)) {
      return false;
    }
    bytes_.push_back(std::byte{value});
    return true;
  }

  [[nodiscard]] bool append_u16(std::uint16_t value) {
    return append_byte(static_cast<std::uint8_t>((value >> 8U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>(value & 0xffU));
  }

  [[nodiscard]] bool append_u32(std::uint32_t value) {
    return append_byte(static_cast<std::uint8_t>((value >> 24U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>((value >> 16U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>((value >> 8U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>(value & 0xffU));
  }

  [[nodiscard]] bool append_u64(std::uint64_t value) {
    for (unsigned int shift = 56U;; shift -= 8U) {
      if (!append_byte(static_cast<std::uint8_t>((value >> shift) & 0xffU))) {
        return false;
      }
      if (shift == 0U) {
        break;
      }
    }
    return true;
  }

  [[nodiscard]] bool append_bytes(std::span<const std::byte> value) {
    if (!can_grow(value.size())) {
      return false;
    }
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
  }

  [[nodiscard]] bool append_field(std::uint16_t field_tag, std::span<const std::byte> payload) {
    if (payload.size() > maximum_u32_size) {
      return false;
    }
    return append_u16(field_tag) && append_u32(static_cast<std::uint32_t>(payload.size())) &&
           append_bytes(payload);
  }

  [[nodiscard]] bool append_ascii_field(std::uint16_t field_tag, std::string_view value) {
    if (value.size() > maximum_u32_size) {
      return false;
    }
    if (!append_u16(field_tag) || !append_u32(static_cast<std::uint32_t>(value.size()))) {
      return false;
    }
    return append_ascii_raw(value);
  }

  [[nodiscard]] bool append_u8_field(std::uint16_t field_tag, std::uint8_t value) {
    const std::array payload{std::byte{value}};
    return append_field(field_tag, payload);
  }

  [[nodiscard]] bool append_u64_field(std::uint16_t field_tag, std::uint64_t value) {
    CanonicalConfigurationWriter payload;
    return payload.append_u64(value) && append_field(field_tag, payload.bytes());
  }

  [[nodiscard]] bool append_decimal_field(std::uint16_t field_tag, std::int64_t coefficient,
                                          std::uint8_t scale) {
    CanonicalConfigurationWriter payload;
    // Conversion from signed to unsigned is defined modulo 2^64 and therefore emits the required
    // fixed-width two's-complement coefficient on every supported platform.
    return payload.append_u64(static_cast<std::uint64_t>(coefficient)) &&
           payload.append_byte(scale) && append_field(field_tag, payload.bytes());
  }

  [[nodiscard]] bool append_length_prefixed(std::span<const std::byte> value) {
    if (value.size() > maximum_u32_size) {
      return false;
    }
    return append_u32(static_cast<std::uint32_t>(value.size())) && append_bytes(value);
  }

  template <typename Records, typename Encode>
  [[nodiscard]] bool append_sequence_field(std::uint16_t field_tag, const Records& records,
                                           Encode encode) {
    if (records.size() > maximum_u32_size) {
      return false;
    }
    CanonicalConfigurationWriter payload;
    if (!payload.append_u32(static_cast<std::uint32_t>(records.size()))) {
      return false;
    }
    for (const auto& record : records) {
      auto encoded_record = encode(record);
      if (!encoded_record || !payload.append_length_prefixed(encoded_record.value().bytes())) {
        return false;
      }
    }
    return append_field(field_tag, payload.bytes());
  }

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::vector<std::byte> take_bytes() && { return std::move(bytes_); }

private:
  static constexpr std::size_t maximum_u32_size =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

  [[nodiscard]] bool can_grow(std::size_t additional_size) const noexcept {
    return additional_size <= bytes_.max_size() - bytes_.size();
  }

  std::vector<std::byte> bytes_;
};

using EncodedRecord = model::Result<CanonicalConfigurationWriter>;

[[nodiscard]] EncodedRecord encoded_record(CanonicalConfigurationWriter writer, bool success) {
  if (!success) {
    return EncodedRecord::failure(
        DomainError::at_field(DomainErrorCode::EncodingOverflow, "configuration.encoding"));
  }
  return EncodedRecord::success(std::move(writer));
}

[[nodiscard]] EncodedRecord encode_firm(const organization::Firm& firm) {
  CanonicalConfigurationWriter writer;
  const bool success = writer.append_ascii_field(1U, firm.id.value());
  return encoded_record(std::move(writer), success);
}

[[nodiscard]] EncodedRecord encode_desk(const organization::Desk& desk) {
  CanonicalConfigurationWriter writer;
  const bool success = writer.append_ascii_field(1U, desk.id.value()) &&
                       writer.append_ascii_field(2U, desk.firm_id.value());
  return encoded_record(std::move(writer), success);
}

[[nodiscard]] EncodedRecord encode_bot(const organization::BotRegistration& bot) {
  CanonicalConfigurationWriter writer;
  const bool success = writer.append_ascii_field(1U, bot.id.value()) &&
                       writer.append_ascii_field(2U, bot.desk_id.value()) &&
                       writer.append_ascii_field(3U, bot.strategy_id.value());
  return encoded_record(std::move(writer), success);
}

[[nodiscard]] EncodedRecord encode_strategy_settings(const BotStrategySettings& settings) {
  CanonicalConfigurationWriter writer;
  const bool success = writer.append_ascii_field(1U, settings.bot_id.value()) &&
                       writer.append_ascii_field(2U, settings.strategy_id.value()) &&
                       writer.append_u8_field(3U, static_cast<std::uint8_t>(settings.mode));
  return encoded_record(std::move(writer), success);
}

[[nodiscard]] EncodedRecord encode_venue(const VenueDefinition& venue) {
  CanonicalConfigurationWriter writer;
  const bool success = writer.append_ascii_field(1U, venue.id.value()) &&
                       writer.append_u8_field(2U, static_cast<std::uint8_t>(venue.environment));
  return encoded_record(std::move(writer), success);
}

[[nodiscard]] EncodedRecord encode_logical_account(const LogicalAccountVenueBinding& binding) {
  CanonicalConfigurationWriter writer;
  const bool success = writer.append_ascii_field(1U, binding.logical_account_id.value()) &&
                       writer.append_ascii_field(2U, binding.firm_id.value()) &&
                       writer.append_ascii_field(3U, binding.venue_id.value());
  return encoded_record(std::move(writer), success);
}

[[nodiscard]] EncodedRecord encode_instrument_metadata(const model::InstrumentMetadata& metadata) {
  CanonicalConfigurationWriter writer;
  const bool success =
      writer.append_ascii_field(1U, metadata.venue_id().value()) &&
      writer.append_ascii_field(2U, metadata.instrument_id().value()) &&
      writer.append_ascii_field(3U, metadata.venue_instrument_id().value()) &&
      writer.append_u64_field(4U, metadata.revision().value()) &&
      writer.append_ascii_field(5U, metadata.base_currency()) &&
      writer.append_ascii_field(6U, metadata.quote_currency()) &&
      writer.append_ascii_field(7U, metadata.settlement_currency()) &&
      writer.append_u8_field(8U, static_cast<std::uint8_t>(metadata.contract_style())) &&
      writer.append_u8_field(9U, static_cast<std::uint8_t>(metadata.quantity_unit())) &&
      writer.append_u8_field(10U, static_cast<std::uint8_t>(metadata.contract_multiplier_unit())) &&
      writer.append_u8_field(11U, metadata.price_scale()) &&
      writer.append_u8_field(12U, metadata.quantity_scale()) &&
      writer.append_decimal_field(13U, metadata.tick_size().coefficient(),
                                  metadata.tick_size().scale()) &&
      writer.append_decimal_field(14U, metadata.quantity_step().coefficient(),
                                  metadata.quantity_step().scale()) &&
      writer.append_decimal_field(15U, metadata.minimum_quantity().coefficient(),
                                  metadata.minimum_quantity().scale()) &&
      writer.append_decimal_field(16U, metadata.contract_multiplier().coefficient(),
                                  metadata.contract_multiplier().scale());
  return encoded_record(std::move(writer), success);
}

[[nodiscard]] EncodedRecord encode_subscription(const market_data::Subscription& subscription) {
  CanonicalConfigurationWriter writer;
  const bool success = writer.append_ascii_field(1U, subscription.id.value()) &&
                       writer.append_ascii_field(2U, subscription.bot_id.value()) &&
                       writer.append_ascii_field(3U, subscription.venue_id.value()) &&
                       writer.append_ascii_field(4U, subscription.instrument_id.value()) &&
                       writer.append_u8_field(5U, static_cast<std::uint8_t>(subscription.channel));
  return encoded_record(std::move(writer), success);
}

[[nodiscard]] EncodedRecord encode_route(const execution::ExecutionRoute& route) {
  CanonicalConfigurationWriter writer;
  const bool success = writer.append_ascii_field(1U, route.id.value()) &&
                       writer.append_ascii_field(2U, route.bot_id.value()) &&
                       writer.append_ascii_field(3U, route.venue_id.value()) &&
                       writer.append_ascii_field(4U, route.logical_account_id.value()) &&
                       writer.append_ascii_field(5U, route.instrument_id.value()) &&
                       writer.append_u8_field(6U, static_cast<std::uint8_t>(route.state));
  return encoded_record(std::move(writer), success);
}

[[nodiscard]] model::Result<std::vector<std::byte>>
encode_configuration(model::ConfigurationRevision revision,
                     const organization::Organization& organization,
                     model::StrategyConfigurationRevision strategy_revision,
                     const std::vector<BotStrategySettings>& strategy_settings,
                     const std::vector<VenueDefinition>& venues,
                     const std::vector<LogicalAccountVenueBinding>& logical_accounts,
                     const std::vector<model::InstrumentMetadata>& instrument_metadata,
                     const market_data::SubscriptionConfiguration& subscriptions,
                     const execution::ExecutionRouteConfiguration& routes) {
  CanonicalConfigurationWriter writer;
  const bool success =
      writer.append_ascii_raw("AEGISCFG") &&
      writer.append_u16(canonical_configuration_schema_version) &&
      writer.append_u64_field(tag(ConfigurationTag::ConfigurationRevision), revision.value()) &&
      writer.append_u64_field(tag(ConfigurationTag::OrganizationRevision),
                              organization.revision().value()) &&
      writer.append_sequence_field(tag(ConfigurationTag::Firms), organization.firms(),
                                   encode_firm) &&
      writer.append_sequence_field(tag(ConfigurationTag::Desks), organization.desks(),
                                   encode_desk) &&
      writer.append_sequence_field(tag(ConfigurationTag::Bots), organization.bots(), encode_bot) &&
      writer.append_u64_field(tag(ConfigurationTag::StrategyConfigurationRevision),
                              strategy_revision.value()) &&
      writer.append_sequence_field(tag(ConfigurationTag::StrategySettings), strategy_settings,
                                   encode_strategy_settings) &&
      writer.append_sequence_field(tag(ConfigurationTag::Venues), venues, encode_venue) &&
      writer.append_sequence_field(tag(ConfigurationTag::LogicalAccounts), logical_accounts,
                                   encode_logical_account) &&
      writer.append_sequence_field(tag(ConfigurationTag::InstrumentMetadata), instrument_metadata,
                                   encode_instrument_metadata) &&
      writer.append_u64_field(tag(ConfigurationTag::SubscriptionRevision),
                              subscriptions.revision().value()) &&
      writer.append_sequence_field(tag(ConfigurationTag::Subscriptions),
                                   subscriptions.subscriptions(), encode_subscription) &&
      writer.append_u64_field(tag(ConfigurationTag::RouteRevision), routes.revision().value()) &&
      writer.append_sequence_field(tag(ConfigurationTag::Routes), routes.routes(), encode_route);
  if (!success) {
    return model::Result<std::vector<std::byte>>::failure(
        DomainError::at_field(DomainErrorCode::EncodingOverflow, "configuration.encoding"));
  }
  return model::Result<std::vector<std::byte>>::success(std::move(writer).take_bytes());
}

template <typename Record, typename IdAccessor>
[[nodiscard]] model::Result<void> reject_adjacent_duplicate_ids(const std::vector<Record>& records,
                                                                std::string_view field,
                                                                IdAccessor id_of) {
  for (std::size_t index = 1U; index < records.size(); ++index) {
    if (id_of(records[index - 1U]) == id_of(records[index])) {
      return model::Result<void>::failure(
          DomainError::at_index(DomainErrorCode::DuplicateIdentifier, std::string{field}, index));
    }
  }
  return model::Result<void>::success();
}

[[nodiscard]] bool contains_venue(const std::vector<VenueDefinition>& venues,
                                  const model::VenueId& venue_id) noexcept {
  const auto found = std::lower_bound(
      venues.begin(), venues.end(), venue_id,
      [](const VenueDefinition& venue, const model::VenueId& target) { return venue.id < target; });
  return found != venues.end() && found->id == venue_id;
}

[[nodiscard]] bool contains_firm(const organization::Organization& organization,
                                 const model::FirmId& firm_id) noexcept {
  const auto& firms = organization.firms();
  const auto found = std::lower_bound(
      firms.begin(), firms.end(), firm_id,
      [](const organization::Firm& firm, const model::FirmId& target) { return firm.id < target; });
  return found != firms.end() && found->id == firm_id;
}

[[nodiscard]] const organization::BotRegistration*
find_bot_registration(const organization::Organization& organization,
                      const model::BotId& bot_id) noexcept {
  const auto& bots = organization.bots();
  const auto found = std::lower_bound(bots.begin(), bots.end(), bot_id,
                                      [](const organization::BotRegistration& bot,
                                         const model::BotId& target) { return bot.id < target; });
  return found != bots.end() && found->id == bot_id ? &*found : nullptr;
}

[[nodiscard]] const BotStrategySettings*
find_settings(const std::vector<BotStrategySettings>& settings,
              const model::BotId& bot_id) noexcept {
  const auto found =
      std::lower_bound(settings.begin(), settings.end(), bot_id,
                       [](const BotStrategySettings& value, const model::BotId& target) {
                         return value.bot_id < target;
                       });
  return found != settings.end() && found->bot_id == bot_id ? &*found : nullptr;
}

[[nodiscard]] model::Result<std::vector<BotStrategySettings>>
validate_strategy_settings(std::vector<BotStrategySettings> settings,
                           const organization::Organization& organization) {
  std::sort(settings.begin(), settings.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.bot_id < rhs.bot_id; });
  const auto duplicates = reject_adjacent_duplicate_ids(
      settings, "strategy_settings.bot_id",
      [](const BotStrategySettings& value) -> const model::BotId& { return value.bot_id; });
  if (!duplicates) {
    return model::Result<std::vector<BotStrategySettings>>::failure(duplicates.error());
  }

  for (std::size_t index = 0U; index < settings.size(); ++index) {
    const auto* const bot = find_bot_registration(organization, settings[index].bot_id);
    if (bot == nullptr) {
      return model::Result<std::vector<BotStrategySettings>>::failure(DomainError::at_index(
          DomainErrorCode::DanglingReference, "strategy_settings.bot_id", index));
    }
    if (settings[index].strategy_id != bot->strategy_id) {
      return model::Result<std::vector<BotStrategySettings>>::failure(DomainError::at_index(
          DomainErrorCode::InvalidRelationship, "strategy_settings.strategy_id", index));
    }
    if (settings[index].mode != StrategyMode::ObserveOnly) {
      return model::Result<std::vector<BotStrategySettings>>::failure(
          DomainError::at_index(DomainErrorCode::InvalidValue, "strategy_settings.mode", index));
    }
  }

  const auto& bots = organization.bots();
  for (std::size_t index = 0U; index < bots.size(); ++index) {
    if (find_settings(settings, bots[index].id) == nullptr) {
      return model::Result<std::vector<BotStrategySettings>>::failure(DomainError::at_index(
          DomainErrorCode::InvalidRelationship, "strategy_settings.missing_bot", index));
    }
  }
  return model::Result<std::vector<BotStrategySettings>>::success(std::move(settings));
}

[[nodiscard]] model::Result<std::vector<VenueDefinition>>
validate_venues(std::vector<VenueDefinition> venues) {
  if (venues.empty()) {
    return model::Result<std::vector<VenueDefinition>>::failure(
        DomainError::at_field(DomainErrorCode::EmptyCollection, "venues"));
  }
  std::sort(venues.begin(), venues.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.id < rhs.id; });
  const auto duplicates = reject_adjacent_duplicate_ids(
      venues, "venues.id",
      [](const VenueDefinition& venue) -> const model::VenueId& { return venue.id; });
  if (!duplicates) {
    return model::Result<std::vector<VenueDefinition>>::failure(duplicates.error());
  }
  for (std::size_t index = 0U; index < venues.size(); ++index) {
    if (venues[index].environment != VenueEnvironment::Testnet) {
      return model::Result<std::vector<VenueDefinition>>::failure(
          DomainError::at_index(DomainErrorCode::InvalidValue, "venues.environment", index));
    }
  }
  return model::Result<std::vector<VenueDefinition>>::success(std::move(venues));
}

[[nodiscard]] model::Result<std::vector<LogicalAccountVenueBinding>>
validate_logical_accounts(std::vector<LogicalAccountVenueBinding> bindings,
                          const std::vector<VenueDefinition>& venues,
                          const organization::Organization& organization) {
  std::sort(bindings.begin(), bindings.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.logical_account_id < rhs.logical_account_id;
  });
  const auto duplicates = reject_adjacent_duplicate_ids(
      bindings, "logical_accounts.id",
      [](const LogicalAccountVenueBinding& binding) -> const model::LogicalAccountId& {
        return binding.logical_account_id;
      });
  if (!duplicates) {
    return model::Result<std::vector<LogicalAccountVenueBinding>>::failure(duplicates.error());
  }
  for (std::size_t index = 0U; index < bindings.size(); ++index) {
    if (!contains_firm(organization, bindings[index].firm_id)) {
      return model::Result<std::vector<LogicalAccountVenueBinding>>::failure(DomainError::at_index(
          DomainErrorCode::DanglingReference, "logical_accounts.firm_id", index));
    }
    if (!contains_venue(venues, bindings[index].venue_id)) {
      return model::Result<std::vector<LogicalAccountVenueBinding>>::failure(DomainError::at_index(
          DomainErrorCode::DanglingReference, "logical_accounts.venue_id", index));
    }
  }
  return model::Result<std::vector<LogicalAccountVenueBinding>>::success(std::move(bindings));
}

[[nodiscard]] const LogicalAccountVenueBinding*
find_logical_account_binding(const std::vector<LogicalAccountVenueBinding>& bindings,
                             const model::LogicalAccountId& logical_account_id) noexcept {
  const auto found = std::lower_bound(
      bindings.begin(), bindings.end(), logical_account_id,
      [](const LogicalAccountVenueBinding& binding, const model::LogicalAccountId& target) {
        return binding.logical_account_id < target;
      });
  return found != bindings.end() && found->logical_account_id == logical_account_id ? &*found
                                                                                    : nullptr;
}

[[nodiscard]] model::Result<void>
validate_route_firm_ownership(const execution::ExecutionRouteConfiguration& routes,
                              const organization::Organization& organization,
                              const std::vector<LogicalAccountVenueBinding>& logical_accounts) {
  for (std::size_t index = 0U; index < routes.routes().size(); ++index) {
    const auto& route = routes.routes()[index];
    const auto* const bot = organization.find_bot(route.bot_id);
    const auto* const account =
        find_logical_account_binding(logical_accounts, route.logical_account_id);
    // The section factory has already proven both references exist. Keep this defensive branch
    // deterministic if that contract is ever accidentally weakened.
    if (bot == nullptr || account == nullptr) {
      return model::Result<void>::failure(
          DomainError::at_index(DomainErrorCode::DanglingReference, "routes.firm", index));
    }
    if (bot->firm_id != account->firm_id) {
      return model::Result<void>::failure(DomainError::at_index(
          DomainErrorCode::InvalidRelationship, "routes.account_firm", index));
    }
  }
  return model::Result<void>::success();
}

[[nodiscard]] model::Result<std::vector<model::InstrumentMetadata>>
validate_instrument_metadata(std::vector<model::InstrumentMetadataParams> metadata_params,
                             const std::vector<VenueDefinition>& venues) {
  if (metadata_params.empty()) {
    return model::Result<std::vector<model::InstrumentMetadata>>::failure(
        DomainError::at_field(DomainErrorCode::EmptyCollection, "instrument_metadata"));
  }
  std::sort(metadata_params.begin(), metadata_params.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.venue_id, lhs.instrument_id) < std::tie(rhs.venue_id, rhs.instrument_id);
  });
  for (std::size_t index = 1U; index < metadata_params.size(); ++index) {
    if (metadata_params[index - 1U].venue_id == metadata_params[index].venue_id &&
        metadata_params[index - 1U].instrument_id == metadata_params[index].instrument_id) {
      return model::Result<std::vector<model::InstrumentMetadata>>::failure(DomainError::at_index(
          DomainErrorCode::DuplicateIdentifier, "instrument_metadata.venue_instrument", index));
    }
  }

  using NativeInstrumentKey = std::pair<model::VenueId, model::VenueInstrumentId>;
  std::set<NativeInstrumentKey> native_instrument_keys;
  std::vector<model::InstrumentMetadata> metadata;
  metadata.reserve(metadata_params.size());
  for (std::size_t index = 0U; index < metadata_params.size(); ++index) {
    auto& params = metadata_params[index];
    if (!contains_venue(venues, params.venue_id)) {
      return model::Result<std::vector<model::InstrumentMetadata>>::failure(DomainError::at_index(
          DomainErrorCode::DanglingReference, "instrument_metadata.venue_id", index));
    }
    if (!native_instrument_keys.emplace(params.venue_id, params.venue_instrument_id).second) {
      return model::Result<std::vector<model::InstrumentMetadata>>::failure(
          DomainError::at_index(DomainErrorCode::DuplicateIdentifier,
                                "instrument_metadata.venue_native_instrument", index));
    }
    auto validated = model::InstrumentMetadata::create(std::move(params));
    if (!validated) {
      auto error = validated.error();
      error.context.collection_index = index;
      return model::Result<std::vector<model::InstrumentMetadata>>::failure(std::move(error));
    }
    metadata.push_back(std::move(validated).value());
  }
  return model::Result<std::vector<model::InstrumentMetadata>>::success(std::move(metadata));
}

[[nodiscard]] std::vector<market_data::VenueInstrumentPair>
venue_instrument_pairs(const std::vector<model::InstrumentMetadata>& metadata) {
  std::vector<market_data::VenueInstrumentPair> pairs;
  pairs.reserve(metadata.size());
  for (const auto& instrument : metadata) {
    pairs.emplace_back(instrument.venue_id(), instrument.instrument_id());
  }
  return pairs;
}

[[nodiscard]] std::vector<execution::LogicalAccountVenueBinding>
execution_account_bindings(const std::vector<LogicalAccountVenueBinding>& bindings) {
  std::vector<execution::LogicalAccountVenueBinding> result;
  result.reserve(bindings.size());
  for (const auto& binding : bindings) {
    result.emplace_back(binding.logical_account_id, binding.venue_id);
  }
  return result;
}

[[nodiscard]] std::vector<InstrumentMetadataRevisionEntry>
metadata_revision_entries(const std::vector<model::InstrumentMetadata>& metadata) {
  std::vector<InstrumentMetadataRevisionEntry> entries;
  entries.reserve(metadata.size());
  for (const auto& instrument : metadata) {
    entries.push_back(InstrumentMetadataRevisionEntry{
        instrument.venue_id(), instrument.instrument_id(), instrument.revision()});
  }
  return entries;
}

} // namespace

model::Result<StartupConfiguration>
StartupConfiguration::create(StartupConfigurationParams params) {
  // Section validation order is a compatibility contract: organization, strategy settings,
  // venues, logical accounts, metadata, subscriptions, routes, then canonical encoding.
  auto organization =
      organization::Organization::create(params.organization_revision, std::move(params.firms),
                                         std::move(params.desks), std::move(params.bots));
  if (!organization) {
    return model::Result<StartupConfiguration>::failure(organization.error());
  }

  auto strategy_settings =
      validate_strategy_settings(std::move(params.strategy_settings), organization.value());
  if (!strategy_settings) {
    return model::Result<StartupConfiguration>::failure(strategy_settings.error());
  }

  auto venues = validate_venues(std::move(params.venues));
  if (!venues) {
    return model::Result<StartupConfiguration>::failure(venues.error());
  }

  auto logical_accounts = validate_logical_accounts(std::move(params.logical_accounts),
                                                    venues.value(), organization.value());
  if (!logical_accounts) {
    return model::Result<StartupConfiguration>::failure(logical_accounts.error());
  }

  auto metadata =
      validate_instrument_metadata(std::move(params.instrument_metadata), venues.value());
  if (!metadata) {
    return model::Result<StartupConfiguration>::failure(metadata.error());
  }

  const auto known_venue_instruments = venue_instrument_pairs(metadata.value());
  auto subscriptions = market_data::SubscriptionConfiguration::create(
      params.subscription_revision, std::move(params.subscriptions), organization.value(),
      known_venue_instruments);
  if (!subscriptions) {
    return model::Result<StartupConfiguration>::failure(subscriptions.error());
  }

  auto routes = execution::ExecutionRouteConfiguration::create(
      params.route_revision, std::move(params.routes), organization.value(),
      std::vector<execution::VenueInstrumentPair>{known_venue_instruments.begin(),
                                                  known_venue_instruments.end()},
      execution_account_bindings(logical_accounts.value()));
  if (!routes) {
    return model::Result<StartupConfiguration>::failure(routes.error());
  }
  const auto route_firm_ownership =
      validate_route_firm_ownership(routes.value(), organization.value(), logical_accounts.value());
  if (!route_firm_ownership) {
    return model::Result<StartupConfiguration>::failure(route_firm_ownership.error());
  }

  auto canonical_bytes = encode_configuration(
      params.revision, organization.value(), params.strategy_configuration_revision,
      strategy_settings.value(), venues.value(), logical_accounts.value(), metadata.value(),
      subscriptions.value(), routes.value());
  if (!canonical_bytes) {
    return model::Result<StartupConfiguration>::failure(canonical_bytes.error());
  }

  ConfigurationFingerprint fingerprint{model::sha256(canonical_bytes.value())};
  ConfigurationProvenance provenance{
      fingerprint,
      params.revision,
      organization.value().revision(),
      params.strategy_configuration_revision,
      subscriptions.value().revision(),
      routes.value().revision(),
      metadata_revision_entries(metadata.value()),
  };

  return model::Result<StartupConfiguration>::success(StartupConfiguration{
      params.revision,
      std::move(organization).value(),
      params.strategy_configuration_revision,
      std::move(strategy_settings).value(),
      std::move(venues).value(),
      std::move(logical_accounts).value(),
      std::move(metadata).value(),
      std::move(subscriptions).value(),
      std::move(routes).value(),
      std::move(canonical_bytes).value(),
      std::move(fingerprint),
      std::move(provenance),
  });
}

const VenueDefinition*
StartupConfiguration::find_venue(const model::VenueId& venue_id) const noexcept {
  const auto found = std::lower_bound(
      venues_.begin(), venues_.end(), venue_id,
      [](const VenueDefinition& venue, const model::VenueId& target) { return venue.id < target; });
  return found != venues_.end() && found->id == venue_id ? &*found : nullptr;
}

const LogicalAccountVenueBinding* StartupConfiguration::find_logical_account(
    const model::LogicalAccountId& logical_account_id) const noexcept {
  const auto found = std::lower_bound(
      logical_accounts_.begin(), logical_accounts_.end(), logical_account_id,
      [](const LogicalAccountVenueBinding& binding, const model::LogicalAccountId& target) {
        return binding.logical_account_id < target;
      });
  return found != logical_accounts_.end() && found->logical_account_id == logical_account_id
             ? &*found
             : nullptr;
}

const BotStrategySettings*
StartupConfiguration::find_strategy_settings(const model::BotId& bot_id) const noexcept {
  return find_settings(strategy_settings_, bot_id);
}

const model::InstrumentMetadata* StartupConfiguration::find_instrument_metadata(
    const model::VenueId& venue_id, const model::InstrumentId& instrument_id) const noexcept {
  const auto key = std::tie(venue_id, instrument_id);
  const auto found =
      std::lower_bound(instrument_metadata_.begin(), instrument_metadata_.end(), key,
                       [](const model::InstrumentMetadata& metadata, const auto& target) {
                         return std::tie(metadata.venue_id(), metadata.instrument_id()) < target;
                       });
  if (found == instrument_metadata_.end() || found->venue_id() != venue_id ||
      found->instrument_id() != instrument_id) {
    return nullptr;
  }
  return &*found;
}

} // namespace aegis::configuration
