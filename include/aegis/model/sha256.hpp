// Purpose: provide a portable SHA-256 digest for canonical AEGIS provenance bytes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aegis::model {

// ########################################################################
// Fixed-size byte and character arrays make digest width part of the type contract. Sha256Hex has
// no terminator, so callers cannot mistake it for an owning C string.
inline constexpr std::size_t sha256_digest_size = 32U;
inline constexpr std::size_t sha256_hex_size = sha256_digest_size * 2U;

using Sha256Digest = std::array<std::byte, sha256_digest_size>;
using Sha256Hex = std::array<char, sha256_hex_size>;

// ########################################################################
// Hash byte sequences without allocation or dependence on host byte order.
class Sha256 final {
public:

  // --------------------------------------------------------
  // Initialize the standard SHA-256 state and an empty streaming buffer.
  Sha256() noexcept;

  // --------------------------------------------------------
  // Incorporate another byte range while retaining any incomplete trailing block.
  void append_bytes(std::span<const std::byte> bytes) noexcept;

  // --------------------------------------------------------
  // Finalization works on a copy, so callers may inspect a prefix and continue hashing.
  [[nodiscard]] Sha256Digest derive_digest() const noexcept;

  // --------------------------------------------------------
private:
  static constexpr std::size_t block_size = 64U;

  // --------------------------------------------------------
  // Interesting syntax: a fixed-extent span makes partial-block compression unrepresentable at the
  // call boundary while borrowing the caller's storage without allocation.
  // Mix exactly one complete block into the running digest state.
  void compress(std::span<const std::byte, block_size> block) noexcept;

  // --------------------------------------------------------
  std::array<std::uint32_t, 8U> state_{};
  std::array<std::byte, block_size> buffer_{};
  std::uint64_t byte_count_{0U};
  std::size_t buffered_bytes_{0U};
};

// ########################################################################

// --------------------------------------------------------
// The one-shot helper uses the same streaming implementation, keeping a single hashing contract.
[[nodiscard]] Sha256Digest calculate_sha256_digest(std::span<const std::byte> bytes) noexcept;

// --------------------------------------------------------
// Return exactly 64 lowercase ASCII hexadecimal digits, without a terminator.
[[nodiscard]] Sha256Hex sha256_hex_from_digest(const Sha256Digest& digest) noexcept;

// --------------------------------------------------------
} // namespace aegis::model
