// Purpose: define stable machine-readable failures for dependency-light domain operations.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace aegis::model {

// Values are persisted compatibility identifiers. The hundreds bands reserve primitive, metadata,
// configuration, and encoding failures; add new codes explicitly and never renumber one.
enum class DomainErrorCode : std::uint16_t {
  InvalidIdentifier = 1,
  InvalidValue = 2,
  InvalidRevision = 3,
  ArithmeticOverflow = 4,
  DivisionByZero = 5,
  PrecisionLoss = 6,
  InvalidDecimal = 7,
  InvalidScale = 8,
  CounterExhausted = 9,
  EntropyUnavailable = 10,
  InvalidTimestampOrder = 11,

  InvalidMetadata = 100,
  MisalignedPrice = 101,
  MisalignedQuantity = 102,

  EmptyCollection = 200,
  DuplicateIdentifier = 201,
  DanglingReference = 202,
  InvalidRelationship = 203,
  ExecutionNotPermitted = 204,

  EncodingOverflow = 300,
  TraceCapacityExceeded = 301,
};

// Human prose is deliberately absent: callers receive a stable field key and optional position.
struct DomainErrorContext {
  std::string field;
  std::optional<std::size_t> collection_index;

  friend bool operator==(const DomainErrorContext&, const DomainErrorContext&) = default;
};

// Named factories make the presence or absence of collection position explicit at each failure
// site, while structural equality lets deterministic tests compare the complete machine contract.
struct DomainError {
  DomainErrorCode code;
  DomainErrorContext context;

  [[nodiscard]] static DomainError at_field(DomainErrorCode code, std::string field) {
    return DomainError{code, DomainErrorContext{std::move(field), std::nullopt}};
  }

  [[nodiscard]] static DomainError at_index(DomainErrorCode code, std::string field,
                                            std::size_t index) {
    return DomainError{code, DomainErrorContext{std::move(field), index}};
  }

  friend bool operator==(const DomainError&, const DomainError&) = default;
};

} // namespace aegis::model
