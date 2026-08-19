// Purpose: provide validated nominal identifiers that cannot be mixed across domain kinds.

#pragma once

#include "aegis/model/result.hpp"

#include <compare>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::model {
namespace detail {

// ########################################################################
// Grammar selection is closed and internal; public identifiers expose validated values, not a
// caller-selectable grammar mode.
enum class IdentifierGrammar { Organization, Venue, Instrument, Adapter };

// ########################################################################
// Interesting syntax: incomplete tag types parameterize one implementation into unrelated nominal
// C++ types, while traits bind each type to its grammar and stable error field at compile time.
// Firm identifiers use a distinct incomplete tag.
struct FirmIdTag;

// ########################################################################
// Desk identifiers use a distinct incomplete tag.
struct DeskIdTag;

// ########################################################################
// Bot identifiers use a distinct incomplete tag.
struct BotIdTag;

// ########################################################################
// Strategy identifiers use a distinct incomplete tag.
struct StrategyIdTag;

// ########################################################################
// Venue identifiers use a distinct incomplete tag.
struct VenueIdTag;

// ########################################################################
// Logical-account identifiers use a distinct incomplete tag.
struct LogicalAccountIdTag;

// ########################################################################
// Instrument identifiers use a distinct incomplete tag.
struct InstrumentIdTag;

// ########################################################################
// Venue-instrument identifiers use a distinct incomplete tag.
struct VenueInstrumentIdTag;

// ########################################################################
// Subscription identifiers use a distinct incomplete tag.
struct SubscriptionIdTag;

// ########################################################################
// Route identifiers use a distinct incomplete tag.
struct RouteIdTag;

// ########################################################################
// Venue-account identifiers use a distinct incomplete tag.
struct VenueAccountIdTag;

// ########################################################################
// Market-source identifiers name one configured normalized ingress stream independently from its
// venue and instrument key.
struct MarketSourceIdTag;

// ########################################################################
// Traits bind each nominal tag to its validation grammar and stable error field.
template <typename Tag> struct IdentifierTraits;

// ########################################################################
// These specializations keep the repeated organization mapping uniform without introducing a
// runtime registry or allowing a prefix to drift away from its error field.
#define AEGIS_ORGANIZATION_ID_TRAITS(TagName, Prefix, Field)                                       \
  template <> struct IdentifierTraits<TagName> {                                                   \
    static constexpr IdentifierGrammar grammar = IdentifierGrammar::Organization;                  \
    static constexpr std::string_view prefix = Prefix;                                             \
    static constexpr std::string_view field = Field;                                               \
  }

// ########################################################################
// Firm traits bind the firm prefix and stable field to the organization grammar.
AEGIS_ORGANIZATION_ID_TRAITS(FirmIdTag, "firm.", "firm_id");

// ########################################################################
// Desk traits bind the desk prefix and stable field to the organization grammar.
AEGIS_ORGANIZATION_ID_TRAITS(DeskIdTag, "desk.", "desk_id");

// ########################################################################
// Bot traits bind the bot prefix and stable field to the organization grammar.
AEGIS_ORGANIZATION_ID_TRAITS(BotIdTag, "bot.", "bot_id");

// ########################################################################
// Strategy traits bind the strategy prefix and stable field to the organization grammar.
AEGIS_ORGANIZATION_ID_TRAITS(StrategyIdTag, "strategy.", "strategy_id");

// ########################################################################
// Logical-account traits bind the account prefix and stable field to the organization grammar.
AEGIS_ORGANIZATION_ID_TRAITS(LogicalAccountIdTag, "account.", "logical_account_id");

// ########################################################################
// Subscription traits bind the subscription prefix and stable field to the organization grammar.
AEGIS_ORGANIZATION_ID_TRAITS(SubscriptionIdTag, "subscription.", "subscription_id");

// ########################################################################
// Route traits bind the route prefix and stable field to the organization grammar.
AEGIS_ORGANIZATION_ID_TRAITS(RouteIdTag, "route.", "route_id");

// ########################################################################
// Market-source traits bind the source prefix and stable field to the organization grammar.
AEGIS_ORGANIZATION_ID_TRAITS(MarketSourceIdTag, "source.", "market_source_id");

// ########################################################################
#undef AEGIS_ORGANIZATION_ID_TRAITS

// ########################################################################
// Venue and normalized-instrument identifiers use distinct exchange-facing grammars without a
// required organizational prefix, while still retaining type-specific error fields.
template <> struct IdentifierTraits<VenueIdTag> {
  static constexpr IdentifierGrammar grammar = IdentifierGrammar::Venue;
  static constexpr std::string_view prefix{};
  static constexpr std::string_view field = "venue_id";
};

// ########################################################################
// Normalized instruments use the instrument grammar and their own stable error field.
template <> struct IdentifierTraits<InstrumentIdTag> {
  static constexpr IdentifierGrammar grammar = IdentifierGrammar::Instrument;
  static constexpr std::string_view prefix{};
  static constexpr std::string_view field = "instrument_id";
};

// ########################################################################
// Adapter-owned strings share a deliberately broad printable-ASCII grammar but remain distinct
// nominal types with distinct failure fields.
#define AEGIS_ADAPTER_ID_TRAITS(TagName, Field)                                                    \
  template <> struct IdentifierTraits<TagName> {                                                   \
    static constexpr IdentifierGrammar grammar = IdentifierGrammar::Adapter;                       \
    static constexpr std::string_view prefix{};                                                    \
    static constexpr std::string_view field = Field;                                               \
  }

// ########################################################################
// Venue-instrument traits bind adapter text to its stable failure field.
AEGIS_ADAPTER_ID_TRAITS(VenueInstrumentIdTag, "venue_instrument_id");

// ########################################################################
// Venue-account traits bind adapter text to its distinct stable failure field.
AEGIS_ADAPTER_ID_TRAITS(VenueAccountIdTag, "venue_account_id");

// ########################################################################
#undef AEGIS_ADAPTER_ID_TRAITS

// ########################################################################

// --------------------------------------------------------
// Validate one candidate against the selected internal grammar and optional prefix.
[[nodiscard]] bool is_valid_identifier(std::string_view value, IdentifierGrammar grammar,
                                       std::string_view prefix) noexcept;

// --------------------------------------------------------
} // namespace detail

// ########################################################################
// Construction is factory-only so every stored string has already passed its type's exact grammar;
// the owning value is then immutable through the public API.
template <typename Tag> class Identifier {
public:

  // --------------------------------------------------------
  // Validate and own one identifier string using the grammar selected by its nominal tag.
  [[nodiscard]] static Result<Identifier> parse(std::string_view value) {

    // ++++++++++++++++++++++++++++++++++++++++
    // Resolve the tag-specific validation and error metadata at compile time.
    using Traits = detail::IdentifierTraits<Tag>;

    // ++++++++++++++++++++++++++++++++++++++++
    // Reject invalid text before allocating the owned identifier value.
    if (!detail::is_valid_identifier(value, Traits::grammar, Traits::prefix)) {
      return Result<Identifier>::failure(
          DomainError::at_field(DomainErrorCode::InvalidIdentifier, std::string{Traits::field}));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Publish only a value that has passed its complete nominal grammar.
    return Result<Identifier>::success(Identifier{std::string{value}});

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Borrow the immutable validated spelling without transferring ownership.
  [[nodiscard]] std::string_view value() const noexcept { return value_; }

  // --------------------------------------------------------
  // Interesting syntax: defaulted hidden friends compare only the same tag instantiation, making
  // cross-domain equality and ordering ill-formed instead of relying on caller discipline.
  friend bool operator==(const Identifier&, const Identifier&) = default;
  friend auto operator<=>(const Identifier&, const Identifier&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Restrict raw-string construction to the validated parse factory.
  explicit Identifier(std::string value) : value_{std::move(value)} {}

  // --------------------------------------------------------
  std::string value_;
};

// ########################################################################
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
using MarketSourceId = Identifier<detail::MarketSourceIdTag>;

// ########################################################################
} // namespace aegis::model
