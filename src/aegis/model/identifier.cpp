// Purpose: enforce the accepted ASCII grammars for every configuration-owned identifier.

#include "aegis/model/identifier.hpp"

#include <cstddef>

namespace aegis::model::detail {
namespace {

[[nodiscard]] bool is_lower_alphanumeric(char character) noexcept {
  return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
}

[[nodiscard]] bool is_upper_alphanumeric(char character) noexcept {
  return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
}

template <typename IsAlphanumeric>
[[nodiscard]] bool is_segmented_token(std::string_view value, char separator,
                                      IsAlphanumeric is_alphanumeric) noexcept {
  if (value.empty() || !is_alphanumeric(value.front()) || !is_alphanumeric(value.back())) {
    return false;
  }

  bool previous_was_separator = false;
  for (const char character : value) {
    if (character == separator) {
      if (previous_was_separator) {
        return false;
      }
      previous_was_separator = true;
      continue;
    }
    if (!is_alphanumeric(character)) {
      return false;
    }
    previous_was_separator = false;
  }
  return true;
}

[[nodiscard]] bool is_organization_identifier(std::string_view value,
                                              std::string_view prefix) noexcept {
  constexpr std::size_t maximum_size = 64;
  if (value.size() > maximum_size || !value.starts_with(prefix)) {
    return false;
  }

  const auto slug = value.substr(prefix.size());
  if (slug.empty() || !is_lower_alphanumeric(slug.front()) || !is_lower_alphanumeric(slug.back())) {
    return false;
  }

  bool previous_was_separator = false;
  for (const char character : slug) {
    const bool is_separator = character == '.' || character == '-';
    if (is_separator) {
      if (previous_was_separator) {
        return false;
      }
      previous_was_separator = true;
      continue;
    }
    if (!is_lower_alphanumeric(character)) {
      return false;
    }
    previous_was_separator = false;
  }
  return true;
}

[[nodiscard]] bool is_adapter_identifier(std::string_view value) noexcept {
  constexpr std::size_t maximum_size = 128;
  if (value.empty() || value.size() > maximum_size) {
    return false;
  }

  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (character < 0x20U || character > 0x7eU) {
      return false;
    }
  }
  return true;
}

} // namespace

bool is_valid_identifier(std::string_view value, IdentifierGrammar grammar,
                         std::string_view prefix) noexcept {
  switch (grammar) {
  case IdentifierGrammar::Organization:
    return is_organization_identifier(value, prefix);
  case IdentifierGrammar::Venue:
    return value.size() <= 32 && is_segmented_token(value, '-', is_lower_alphanumeric);
  case IdentifierGrammar::Instrument:
    return value.size() <= 64 && is_segmented_token(value, '-', is_upper_alphanumeric);
  case IdentifierGrammar::Adapter:
    return is_adapter_identifier(value);
  }
  return false;
}

} // namespace aegis::model::detail
