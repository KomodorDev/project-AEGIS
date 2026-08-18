// Purpose: constrain checked numeric entry points to portable standard integer source types.

#pragma once

#include <concepts>
#include <type_traits>

namespace aegis::model::detail {

template <typename Value> using UnqualifiedIntegerInput = std::remove_cvref_t<Value>;

// std::in_range is specified for the standard signed and unsigned integer types, excluding bool,
// plain char, and the wide/Unicode character types. Matching that set keeps requires-expressions
// and real calls aligned; signed char and unsigned char remain ordinary integer inputs.
template <typename Value>
concept CheckedIntegerInput = std::same_as<UnqualifiedIntegerInput<Value>, signed char> ||
                              std::same_as<UnqualifiedIntegerInput<Value>, unsigned char> ||
                              std::same_as<UnqualifiedIntegerInput<Value>, short> ||
                              std::same_as<UnqualifiedIntegerInput<Value>, unsigned short> ||
                              std::same_as<UnqualifiedIntegerInput<Value>, int> ||
                              std::same_as<UnqualifiedIntegerInput<Value>, unsigned int> ||
                              std::same_as<UnqualifiedIntegerInput<Value>, long> ||
                              std::same_as<UnqualifiedIntegerInput<Value>, unsigned long> ||
                              std::same_as<UnqualifiedIntegerInput<Value>, long long> ||
                              std::same_as<UnqualifiedIntegerInput<Value>, unsigned long long>;

template <typename Value>
concept CheckedUnsignedIntegerInput =
    CheckedIntegerInput<Value> && std::unsigned_integral<UnqualifiedIntegerInput<Value>>;

} // namespace aegis::model::detail
