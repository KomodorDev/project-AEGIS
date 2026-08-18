// Purpose: provide validated nominal identifiers that cannot be mixed across domain kinds.

#pragma once

#include "aegis/model/result.hpp"

#include <compare>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::model {
namespace detail {

// Grammar selection is closed and internal; public identifiers expose validated values, not a
// caller-selectable grammar mode.
enum class IdentifierGrammar { Organization, Venue, Instrument, Adapter };

// Interesting syntax: incomplete tag types parameterize one implementation into unrelated nominal
// C++ types, while traits bind each type to its grammar and stable error field at compile time.
struct FirmIdTag;
struct DeskIdTag;
struct BotIdTag;
struct StrategyIdTag;
struct VenueIdTag;
struct LogicalAccountIdTag;
struct InstrumentIdTag;
struct VenueInstrumentIdTag;
struct SubscriptionIdTag;
struct RouteIdTag;
struct VenueAccountIdTag;

template <typename Tag> struct IdentifierTraits;

// These specializations keep the repeated organization mapping uniform without introducing a
// runtime registry or allowing a prefix to drift away from its error field.
#define AEGIS_ORGANIZATION_ID_TRAITS(TagName, Prefix, Field)                                       \
  template <> struct IdentifierTraits<TagName> {                                                   \
    static constexpr IdentifierGrammar grammar = IdentifierGrammar::Organization;                  \
    static constexpr std::string_view prefix = Prefix;                                             \
    static constexpr std::string_view field = Field;                                               \
  }

AEGIS_ORGANIZATION_ID_TRAITS(FirmIdTag, "firm.", "firm_id");
AEGIS_ORGANIZATION_ID_TRAITS(DeskIdTag, "desk.", "desk_id");
AEGIS_ORGANIZATION_ID_TRAITS(BotIdTag, "bot.", "bot_id");
AEGIS_ORGANIZATION_ID_TRAITS(StrategyIdTag, "strategy.", "strategy_id");
AEGIS_ORGANIZATION_ID_TRAITS(LogicalAccountIdTag, "account.", "logical_account_id");
AEGIS_ORGANIZATION_ID_TRAITS(SubscriptionIdTag, "subscription.", "subscription_id");
AEGIS_ORGANIZATION_ID_TRAITS(RouteIdTag, "route.", "route_id");

#undef AEGIS_ORGANIZATION_ID_TRAITS

// Venue and normalized-instrument identifiers use distinct exchange-facing grammars without a
// required organizational prefix, while still retaining type-specific error fields.
template <> struct IdentifierTraits<VenueIdTag> {
  static constexpr IdentifierGrammar grammar = IdentifierGrammar::Venue;
  static constexpr std::string_view prefix{};
  static constexpr std::string_view field = "venue_id";
};

template <> struct IdentifierTraits<InstrumentIdTag> {
  static constexpr IdentifierGrammar grammar = IdentifierGrammar::Instrument;
  static constexpr std::string_view prefix{};
  static constexpr std::string_view field = "instrument_id";
};

// Adapter-owned strings share a deliberately broad printable-ASCII grammar but remain distinct
// nominal types with distinct failure fields.
#define AEGIS_ADAPTER_ID_TRAITS(TagName, Field)                                                    \
  template <> struct IdentifierTraits<TagName> {                                                   \
    static constexpr IdentifierGrammar grammar = IdentifierGrammar::Adapter;                       \
    static constexpr std::string_view prefix{};                                                    \
    static constexpr std::string_view field = Field;                                               \
  }

AEGIS_ADAPTER_ID_TRAITS(VenueInstrumentIdTag, "venue_instrument_id");
AEGIS_ADAPTER_ID_TRAITS(VenueAccountIdTag, "venue_account_id");

#undef AEGIS_ADAPTER_ID_TRAITS

[[nodiscard]] bool is_valid_identifier(std::string_view value, IdentifierGrammar grammar,
                                       std::string_view prefix) noexcept;

} // namespace detail

// Construction is factory-only so every stored string has already passed its type's exact grammar;
// the owning value is then immutable through the public API.
template <typename Tag> class Identifier {
public:
  [[nodiscard]] static Result<Identifier> parse(std::string_view value) {
    using Traits = detail::IdentifierTraits<Tag>;
    if (!detail::is_valid_identifier(value, Traits::grammar, Traits::prefix)) {
      return Result<Identifier>::failure(
          DomainError::at_field(DomainErrorCode::InvalidIdentifier, std::string{Traits::field}));
    }
    return Result<Identifier>::success(Identifier{std::string{value}});
  }

  [[nodiscard]] std::string_view value() const noexcept { return value_; }

  // Interesting syntax: defaulted hidden friends compare only the same tag instantiation, making
  // cross-domain equality and ordering ill-formed instead of relying on caller discipline.
  friend bool operator==(const Identifier&, const Identifier&) = default;
  friend auto operator<=>(const Identifier&, const Identifier&) = default;

private:
  explicit Identifier(std::string value) : value_{std::move(value)} {}

  std::string value_;
};

// Public aliases are the domain vocabulary; the shared implementation is intentionally not exposed
// as a generic string identifier at call sites.
using FirmId = Identifier<detail::FirmIdTag>;
using DeskId = Identifier<detail::DeskIdTag>;
using BotId = Identifier<detail::BotIdTag>;
using StrategyId = Identifier<detail::StrategyIdTag>;
using VenueId = Identifier<detail::VenueIdTag>;
using LogicalAccountId = Identifier<detail::LogicalAccountIdTag>;
using InstrumentId = Identifier<detail::InstrumentIdTag>;
using VenueInstrumentId = Identifier<detail::VenueInstrumentIdTag>;
using SubscriptionId = Identifier<detail::SubscriptionIdTag>;
using RouteId = Identifier<detail::RouteIdTag>;
using VenueAccountId = Identifier<detail::VenueAccountIdTag>;

} // namespace aegis::model
