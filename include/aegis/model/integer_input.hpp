// Purpose: define the portable integer source-type concepts used before checked narrowing; exclude
// ambiguous character, Boolean, enum, and floating inputs from authored numeric APIs.

#pragma once

#include <concepts>
#include <type_traits>

namespace aegis::model::detail {

// ########################################################################
// This alias-and-concept family defines the only portable integer source types accepted before
// checked narrowing at public domain boundaries.
// Interesting syntax: remove_cvref makes concept membership depend on the underlying source type,
// not on whether a caller or requires-expression presents it through cv/ref qualification.
template <typename Value> using UnqualifiedIntegerInput = std::remove_cvref_t<Value>;

// Interesting syntax: enumerate exactly the standard signed/unsigned types accepted by
// std::in_range. This keeps requires-expressions and instantiated bodies aligned: bool,
// plain/wide/Unicode character types, enums, and floating-point values are excluded, while signed
// char and unsigned char remain ordinary numeric integer inputs.
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

// Non-fallible constructors further require an unsigned member of that portable set. This admits
// unsigned char but not signed char because there is no Result channel for rejecting a negative.
template <typename Value>
concept CheckedUnsignedIntegerInput =
    CheckedIntegerInput<Value> && std::unsigned_integral<UnqualifiedIntegerInput<Value>>;

// ########################################################################
} // namespace aegis::model::detail
