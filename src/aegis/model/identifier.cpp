// Purpose: enforce the accepted ASCII grammars for every configuration-owned identifier.

#include "aegis/model/identifier.hpp"

#include <cstddef>

namespace aegis::model::detail {
namespace {

// --------------------------------------------------------
// Explicit byte ranges keep identifier acceptance independent of locale and character-class APIs.
[[nodiscard]] bool is_lower_alphanumeric(char character) noexcept {
  return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
}

// --------------------------------------------------------
// Apply the equivalent locale-independent predicate to the uppercase identifier grammars.
[[nodiscard]] bool is_upper_alphanumeric(char character) noexcept {
  return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
}

// --------------------------------------------------------
// Validate a token as non-empty alphanumeric segments separated by one accepted byte.
template <typename IsAlphanumeric>
[[nodiscard]] bool is_segmented_token(std::string_view value, char separator,
                                      IsAlphanumeric is_alphanumeric) noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Endpoints must be data, and the scan then rules out adjacent separators and foreign bytes.
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

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Validate prefixed firm, desk, and bot identifiers under their shared slug grammar.
[[nodiscard]] bool is_organization_identifier(std::string_view value,
                                              std::string_view prefix) noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // The stable prefix counts toward the limit; only the remaining slug may contain dot or dash
  // separators, with the same non-empty-segment invariant as other token grammars.
  if (value.size() > maximum_organization_identifier_size || !value.starts_with(prefix)) {
    return false;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate the prefix-stripped slug endpoints before scanning its internal separators.
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

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Accept printable venue-native adapter identifiers without locale-sensitive classification.
[[nodiscard]] bool is_adapter_identifier(std::string_view value) noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Adapter identifiers preserve venue punctuation but reject controls, non-ASCII bytes, and values
  // too large for the fixed configuration contract.
  if (value.empty() || value.size() > maximum_adapter_identifier_size) {
    return false;
  }

  for (const char raw_character : value) {
    // Interesting syntax: converting through unsigned char prevents implementation-defined signed
    // char values from satisfying or bypassing the byte-range comparison.
    const auto character = static_cast<unsigned char>(raw_character);
    if (character < 0x20U || character > 0x7eU) {
      return false;
    }
  }
  return true;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Dispatch each public identifier family to its exact accepted grammar and size bound.
bool is_valid_identifier(std::string_view value, IdentifierGrammar grammar,
                         std::string_view prefix) noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Keep dispatch exhaustive and fail closed if an unassigned enum value reaches this boundary.
  switch (grammar) {
  case IdentifierGrammar::Organization:
    return is_organization_identifier(value, prefix);
  case IdentifierGrammar::Venue:
    return value.size() <= maximum_venue_identifier_size &&
           is_segmented_token(value, '-', is_lower_alphanumeric);
  case IdentifierGrammar::Instrument:
    return value.size() <= maximum_instrument_identifier_size &&
           is_segmented_token(value, '-', is_upper_alphanumeric);
  case IdentifierGrammar::Adapter:
    return is_adapter_identifier(value);
  }
  return false;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::model::detail
