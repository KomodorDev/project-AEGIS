// Purpose: return either a domain value or a stable domain error without an external dependency.

#pragma once

#include "aegis/model/domain_error.hpp"

#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace aegis::model {
// ########################################################################
// A result has exactly one active arm. Reading the inactive arm is a programming error; expected
// domain failures must be inspected through the boolean state and returned as DomainError values.
template <typename T> class [[nodiscard]] Result {
public:
  // --------------------------------------------------------
  // Construct the successful arm while transferring ownership of the domain value.
  [[nodiscard]] static Result success(T value) { return Result{std::move(value)}; }
  // --------------------------------------------------------
  // Construct the failed arm while preserving the complete stable domain error.
  [[nodiscard]] static Result failure(DomainError error) { return Result{std::move(error)}; }
  // --------------------------------------------------------
  // Inspect which variant arm is active without accessing its payload.
  [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
  // --------------------------------------------------------
  // Support concise branching while retaining explicit conversion semantics.
  explicit operator bool() const noexcept { return has_value(); }
  // --------------------------------------------------------
  // Interesting syntax: ref-qualified accessors preserve borrowing from lvalues and permit moving
  // a value or error out of a temporary result without adding a second ownership API.
  [[nodiscard]] T& value() & { return std::get<T>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }
  // --------------------------------------------------------
  // Mirror value access for the active error arm while preserving value category.
  [[nodiscard]] DomainError& error() & { return std::get<DomainError>(storage_); }
  [[nodiscard]] const DomainError& error() const& { return std::get<DomainError>(storage_); }
  [[nodiscard]] DomainError&& error() && { return std::get<DomainError>(std::move(storage_)); }
  // --------------------------------------------------------
private:
  // --------------------------------------------------------
  // Select the successful variant arm explicitly.
  explicit Result(T value) : storage_{std::in_place_type<T>, std::move(value)} {}
  // --------------------------------------------------------
  // Select the failed variant arm explicitly.
  explicit Result(DomainError error)
      : storage_{std::in_place_type<DomainError>, std::move(error)} {}
  // --------------------------------------------------------
  std::variant<T, DomainError> storage_;
};
// ########################################################################
// Interesting syntax: the explicit void specialization uses an optional error as its discriminant,
// so successful commands need no dummy payload while retaining the same inspection contract.
template <> class [[nodiscard]] Result<void> {
public:
  // --------------------------------------------------------
  // Construct success as the absence of an error.
  [[nodiscard]] static Result success() noexcept { return Result{}; }
  // --------------------------------------------------------
  // Construct failure as ownership of one stable domain error.
  [[nodiscard]] static Result failure(DomainError error) { return Result{std::move(error)}; }
  // --------------------------------------------------------
  // Inspect the optional discriminant without accessing its payload.
  [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
  // --------------------------------------------------------
  // Support concise branching while retaining explicit conversion semantics.
  explicit operator bool() const noexcept { return has_value(); }
  // --------------------------------------------------------
  // Treat reading a failed void result as the same programming error as reading an inactive
  // variant.
  void value() const {
    if (!has_value()) {
      throw std::logic_error{"attempted to read a failed Result<void>"};
    }
  }
  // --------------------------------------------------------
  // Borrow or move the active error while preserving value category.
  [[nodiscard]] DomainError& error() & { return error_.value(); }
  [[nodiscard]] const DomainError& error() const& { return error_.value(); }
  [[nodiscard]] DomainError&& error() && { return std::move(error_).value(); }
  // --------------------------------------------------------
private:
  // --------------------------------------------------------
  // Private construction keeps callers on the named success and failure factories.
  Result() = default;
  explicit Result(DomainError error) : error_{std::move(error)} {}
  // --------------------------------------------------------
  std::optional<DomainError> error_;
};
// ########################################################################
} // namespace aegis::model
