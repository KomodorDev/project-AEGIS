// Purpose: validate organizational roots and derive immutable bot attribution.

#include "aegis/organization/organization.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegis::organization {
namespace {

using model::DomainError;
using model::DomainErrorCode;

template <typename Record, typename IdAccessor>
[[nodiscard]] model::Result<void> reject_duplicate_ids(const std::vector<Record>& records,
                                                       std::string_view field, IdAccessor id_of) {
  for (std::size_t index = 1U; index < records.size(); ++index) {
    if (id_of(records[index - 1U]) == id_of(records[index])) {
      return model::Result<void>::failure(
          DomainError::at_index(DomainErrorCode::DuplicateIdentifier, std::string{field}, index));
    }
  }
  return model::Result<void>::success();
}

template <typename Record, typename Id>
[[nodiscard]] const Record* find_by_id(const std::vector<Record>& records, const Id& id) noexcept {
  const auto found =
      std::lower_bound(records.begin(), records.end(), id,
                       [](const Record& record, const Id& target) { return record.id < target; });
  if (found == records.end() || found->id != id) {
    return nullptr;
  }
  return &*found;
}

} // namespace

model::Result<Organization> Organization::create(model::OrganizationRevision revision,
                                                 std::vector<Firm> firms, std::vector<Desk> desks,
                                                 std::vector<BotRegistration> bots) {
  if (firms.empty()) {
    return model::Result<Organization>::failure(
        DomainError::at_field(DomainErrorCode::EmptyCollection, "organization.firms"));
  }
  if (desks.empty()) {
    return model::Result<Organization>::failure(
        DomainError::at_field(DomainErrorCode::EmptyCollection, "organization.desks"));
  }
  if (bots.empty()) {
    return model::Result<Organization>::failure(
        DomainError::at_field(DomainErrorCode::EmptyCollection, "organization.bots"));
  }

  std::sort(firms.begin(), firms.end(),
            [](const Firm& lhs, const Firm& rhs) { return lhs.id < rhs.id; });
  std::sort(desks.begin(), desks.end(),
            [](const Desk& lhs, const Desk& rhs) { return lhs.id < rhs.id; });
  std::sort(bots.begin(), bots.end(),
            [](const BotRegistration& lhs, const BotRegistration& rhs) { return lhs.id < rhs.id; });

  const auto firm_duplicates =
      reject_duplicate_ids(firms, "organization.firms.id",
                           [](const Firm& firm) -> const model::FirmId& { return firm.id; });
  if (!firm_duplicates) {
    return model::Result<Organization>::failure(firm_duplicates.error());
  }
  const auto desk_duplicates =
      reject_duplicate_ids(desks, "organization.desks.id",
                           [](const Desk& desk) -> const model::DeskId& { return desk.id; });
  if (!desk_duplicates) {
    return model::Result<Organization>::failure(desk_duplicates.error());
  }
  const auto bot_duplicates = reject_duplicate_ids(
      bots, "organization.bots.id",
      [](const BotRegistration& bot) -> const model::BotId& { return bot.id; });
  if (!bot_duplicates) {
    return model::Result<Organization>::failure(bot_duplicates.error());
  }

  for (std::size_t index = 0U; index < desks.size(); ++index) {
    if (find_by_id(firms, desks[index].firm_id) == nullptr) {
      return model::Result<Organization>::failure(DomainError::at_index(
          DomainErrorCode::DanglingReference, "organization.desks.firm_id", index));
    }
  }
  for (std::size_t index = 0U; index < bots.size(); ++index) {
    if (find_by_id(desks, bots[index].desk_id) == nullptr) {
      return model::Result<Organization>::failure(DomainError::at_index(
          DomainErrorCode::DanglingReference, "organization.bots.desk_id", index));
    }
  }

  for (std::size_t index = 0U; index < firms.size(); ++index) {
    const bool has_desk = std::any_of(desks.begin(), desks.end(), [&](const Desk& desk) {
      return desk.firm_id == firms[index].id;
    });
    if (!has_desk) {
      return model::Result<Organization>::failure(DomainError::at_index(
          DomainErrorCode::InvalidRelationship, "organization.firms.desks", index));
    }
  }
  for (std::size_t index = 0U; index < desks.size(); ++index) {
    const bool has_bot = std::any_of(bots.begin(), bots.end(), [&](const BotRegistration& bot) {
      return bot.desk_id == desks[index].id;
    });
    if (!has_bot) {
      return model::Result<Organization>::failure(DomainError::at_index(
          DomainErrorCode::InvalidRelationship, "organization.desks.bots", index));
    }
  }

  std::vector<BotAttribution> attributions;
  attributions.reserve(bots.size());
  for (const BotRegistration& bot : bots) {
    const Desk* const desk = find_by_id(desks, bot.desk_id);
    const Firm* const firm = find_by_id(firms, desk->firm_id);
    attributions.push_back(BotAttribution{bot.id, bot.desk_id, firm->id, bot.strategy_id});
  }

  return model::Result<Organization>::success(Organization{
      revision, std::move(firms), std::move(desks), std::move(bots), std::move(attributions)});
}

const BotAttribution* Organization::find_bot(const model::BotId& bot_id) const noexcept {
  const auto found =
      std::lower_bound(bot_attributions_.begin(), bot_attributions_.end(), bot_id,
                       [](const BotAttribution& attribution, const model::BotId& target) {
                         return attribution.bot_id < target;
                       });
  if (found == bot_attributions_.end() || found->bot_id != bot_id) {
    return nullptr;
  }
  return &*found;
}

} // namespace aegis::organization
