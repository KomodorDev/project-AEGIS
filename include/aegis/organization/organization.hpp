// Purpose: model immutable multi-firm organizational attribution for configured bots.

#pragma once

#include "aegis/model/identifier.hpp"
#include "aegis/model/time.hpp"

#include <utility>
#include <vector>

namespace aegis::organization {
// ########################################################################
// Authoring records form complete peer Firm -> Desk -> Bot trees. A firm is one independent root;
// no parent-company aggregation is implied.
struct Firm {
  model::FirmId id;
  // --------------------------------------------------------
  // Structural equality compares the complete firm registration.
  friend bool operator==(const Firm&, const Firm&) = default;
  // --------------------------------------------------------
};
// ########################################################################
// A desk belongs to exactly one peer firm root.
struct Desk {
  model::DeskId id;
  model::FirmId firm_id;
  // --------------------------------------------------------
  // Structural equality compares the desk identity and its owning firm.
  friend bool operator==(const Desk&, const Desk&) = default;
  // --------------------------------------------------------
};
// ########################################################################
// A bot belongs to exactly one desk and carries exactly one strategy registration.
struct BotRegistration {
  model::BotId id;
  model::DeskId desk_id;
  model::StrategyId strategy_id;
  // --------------------------------------------------------
  // Structural equality compares the complete authored bot registration.
  friend bool operator==(const BotRegistration&, const BotRegistration&) = default;
  // --------------------------------------------------------
};
// ########################################################################
// This value is derived from the authoritative registrations and never supplied by a caller.
struct BotAttribution {
  model::BotId bot_id;
  model::DeskId desk_id;
  model::FirmId firm_id;
  model::StrategyId strategy_id;
  // --------------------------------------------------------
  // Structural equality compares the complete derived attribution chain.
  friend bool operator==(const BotAttribution&, const BotAttribution&) = default;
  // --------------------------------------------------------
};
// ########################################################################
// The factory is the only publication boundary: it canonicalizes registrations, rejects incomplete
// trees, and exposes immutable transitive attribution for downstream authorization decisions.
class Organization {
public:
  // --------------------------------------------------------
  // Validate, canonicalize, and atomically publish a complete organization snapshot.
  [[nodiscard]] static model::Result<Organization> create(model::OrganizationRevision revision,
                                                          std::vector<Firm> firms,
                                                          std::vector<Desk> desks,
                                                          std::vector<BotRegistration> bots);
  // --------------------------------------------------------
  // Return the accepted organization-section revision.
  [[nodiscard]] model::OrganizationRevision revision() const noexcept { return revision_; }
  // --------------------------------------------------------
  // Borrow canonical peer-firm roots.
  [[nodiscard]] const std::vector<Firm>& firms() const noexcept { return firms_; }
  // --------------------------------------------------------
  // Borrow canonical desks and their firm ownership.
  [[nodiscard]] const std::vector<Desk>& desks() const noexcept { return desks_; }
  // --------------------------------------------------------
  // Borrow canonical bot registrations.
  [[nodiscard]] const std::vector<BotRegistration>& bots() const noexcept { return bots_; }
  // --------------------------------------------------------
  // Borrow canonical transitive bot attributions.
  [[nodiscard]] const std::vector<BotAttribution>& bot_attributions() const noexcept {
    return bot_attributions_;
  }
  // --------------------------------------------------------
  // Find the immutable transitive attribution for one configured bot.
  [[nodiscard]] const BotAttribution* find_bot(const model::BotId& bot_id) const noexcept;
  // --------------------------------------------------------
  // Structural equality compares the entire published organization snapshot.
  friend bool operator==(const Organization&, const Organization&) = default;
  // --------------------------------------------------------
private:
  // --------------------------------------------------------
  // Assemble only collections that the public factory has already validated and canonicalized.
  Organization(model::OrganizationRevision revision, std::vector<Firm> firms,
               std::vector<Desk> desks, std::vector<BotRegistration> bots,
               std::vector<BotAttribution> bot_attributions)
      : revision_{revision}, firms_{std::move(firms)}, desks_{std::move(desks)},
        bots_{std::move(bots)}, bot_attributions_{std::move(bot_attributions)} {}
  // --------------------------------------------------------
  model::OrganizationRevision revision_;
  std::vector<Firm> firms_;
  std::vector<Desk> desks_;
  std::vector<BotRegistration> bots_;
  std::vector<BotAttribution> bot_attributions_;
};
// ########################################################################
} // namespace aegis::organization
