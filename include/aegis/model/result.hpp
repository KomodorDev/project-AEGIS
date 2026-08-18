// Purpose: return either a domain value or a stable domain error without an external dependency.

#pragma once

#include "aegis/model/domain_error.hpp"

#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace aegis::model {

// A result has exactly one active arm. Reading the inactive arm is a programming error; expected
// domain failures must be inspected through the boolean state and returned as DomainError values.
template <typename T> class [[nodiscard]] Result {
public:
  [[nodiscard]] static Result success(T value) { return Result{std::move(value)}; }

  [[nodiscard]] static Result failure(DomainError error) { return Result{std::move(error)}; }

  [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }

  explicit operator bool() const noexcept { return has_value(); }

  // Interesting syntax: ref-qualified accessors preserve borrowing from lvalues and permit moving
  // a value or error out of a temporary result without adding a second ownership API.
  [[nodiscard]] T& value() & { return std::get<T>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }

  [[nodiscard]] DomainError& error() & { return std::get<DomainError>(storage_); }
  [[nodiscard]] const DomainError& error() const& { return std::get<DomainError>(storage_); }
  [[nodiscard]] DomainError&& error() && { return std::get<DomainError>(std::move(storage_)); }

private:
  explicit Result(T value) : storage_{std::in_place_type<T>, std::move(value)} {}
  explicit Result(DomainError error)
      : storage_{std::in_place_type<DomainError>, std::move(error)} {}

  std::variant<T, DomainError> storage_;
};

// Interesting syntax: the explicit void specialization uses an optional error as its discriminant,
// so successful commands need no dummy payload while retaining the same inspection contract.
template <> class [[nodiscard]] Result<void> {
public:
  [[nodiscard]] static Result success() noexcept { return Result{}; }

  [[nodiscard]] static Result failure(DomainError error) { return Result{std::move(error)}; }

  [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }

  explicit operator bool() const noexcept { return has_value(); }

  void value() const {
    if (!has_value()) {
      throw std::logic_error{"attempted to read a failed Result<void>"};
    }
  }

  [[nodiscard]] DomainError& error() & { return error_.value(); }
  [[nodiscard]] const DomainError& error() const& { return error_.value(); }
  [[nodiscard]] DomainError&& error() && { return std::move(error_).value(); }

private:
  Result() = default;
  explicit Result(DomainError error) : error_{std::move(error)} {}

  std::optional<DomainError> error_;
};

} // namespace aegis::model
