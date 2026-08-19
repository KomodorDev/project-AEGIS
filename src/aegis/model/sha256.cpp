// Purpose: implement the FIPS 180-4 SHA-256 transform for canonical provenance data.

#include "aegis/model/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aegis::model {
namespace {

// These are the 64 round constants assigned by FIPS 180-4; their order is part of the algorithm.
constexpr std::array<std::uint32_t, 64U> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};
// --------------------------------------------------------
// Explicit big-endian loads and stores make message and digest bytes independent of host endian.
[[nodiscard]] constexpr std::uint32_t
load_big_endian_word(std::span<const std::byte, 4U> bytes) noexcept {
  return (std::to_integer<std::uint32_t>(bytes[0U]) << 24U) |
         (std::to_integer<std::uint32_t>(bytes[1U]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[2U]) << 8U) |
         std::to_integer<std::uint32_t>(bytes[3U]);
}
// --------------------------------------------------------
// Store one digest word in network order without exposing host representation.
void store_big_endian_word(std::uint32_t word, std::span<std::byte, 4U> destination) noexcept {
  destination[0U] = static_cast<std::byte>((word >> 24U) & 0xffU);
  destination[1U] = static_cast<std::byte>((word >> 16U) & 0xffU);
  destination[2U] = static_cast<std::byte>((word >> 8U) & 0xffU);
  destination[3U] = static_cast<std::byte>(word & 0xffU);
}
// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Initialize the eight working words to the SHA-256 initial hash values from FIPS 180-4.
Sha256::Sha256() noexcept
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
             0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}
// --------------------------------------------------------
// Streaming updates preserve chunk-boundary independence while retaining at most one partial block.
void Sha256::update(std::span<const std::byte> bytes) noexcept {
  // ++++++++++++++++++++++++++++++++++++++++
  // Count original stream bytes across chunk boundaries; unsigned arithmetic supplies SHA-256's
  // specified modulo-2^64 length behavior.
  byte_count_ += static_cast<std::uint64_t>(bytes.size());
  // ++++++++++++++++++++++++++++++++++++++++
  // Complete a previously buffered block before consuming directly from the new input.
  if (buffered_bytes_ != 0U) {
    const auto available = block_size - buffered_bytes_;
    const auto copied = std::min(available, bytes.size());
    std::copy_n(bytes.begin(), copied,
                buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_bytes_));
    buffered_bytes_ += copied;
    bytes = bytes.subspan(copied);

    if (buffered_bytes_ != block_size) {
      return;
    }
    compress(std::span<const std::byte, block_size>{buffer_});
    buffered_bytes_ = 0U;
  }
  // ++++++++++++++++++++++++++++++++++++++++
  // Compress full caller-owned blocks without copying them through the tail buffer.
  while (bytes.size() >= block_size) {
    compress(std::span<const std::byte, block_size>{bytes.data(), block_size});
    bytes = bytes.subspan(block_size);
  }
  // ++++++++++++++++++++++++++++++++++++++++
  // Retain only the final partial block for the next update or finalization.
  std::copy(bytes.begin(), bytes.end(), buffer_.begin());
  buffered_bytes_ = bytes.size();
  // ++++++++++++++++++++++++++++++++++++++++
}
// --------------------------------------------------------
// Finalization applies standard padding to a snapshot and returns canonical digest bytes.
Sha256Digest Sha256::finalize() const noexcept {
  // ++++++++++++++++++++++++++++++++++++++++
  // Snapshot state so callers may inspect a prefix digest and then continue updating the original.
  auto final_hash = *this;
  const auto bit_count = final_hash.byte_count_ * 8U;
  // ++++++++++++++++++++++++++++++++++++++++
  // Append the mandatory one bit and enough zeroes to leave eight bytes in the final block. Inputs
  // at or beyond byte 56 require a second padding block.
  std::array<std::byte, block_size> padding{};
  padding[0U] = std::byte{0x80U};
  const auto padding_size = final_hash.buffered_bytes_ < 56U ? 56U - final_hash.buffered_bytes_
                                                             : 120U - final_hash.buffered_bytes_;
  final_hash.update(std::span<const std::byte>{padding.data(), padding_size});
  // ++++++++++++++++++++++++++++++++++++++++
  // Append the pre-padding message length as the required 64-bit big-endian bit count.
  std::array<std::byte, 8U> encoded_bit_count{};
  for (std::size_t index = 0U; index < encoded_bit_count.size(); ++index) {
    const auto shift = static_cast<unsigned int>((encoded_bit_count.size() - 1U - index) * 8U);
    encoded_bit_count[index] = static_cast<std::byte>((bit_count >> shift) & 0xffU);
  }
  final_hash.update(encoded_bit_count);
  // ++++++++++++++++++++++++++++++++++++++++
  // Serialize working words in canonical network order rather than exposing host representation.
  Sha256Digest digest{};
  for (std::size_t index = 0U; index < final_hash.state_.size(); ++index) {
    store_big_endian_word(final_hash.state_[index],
                          std::span<std::byte, 4U>{digest.data() + (index * 4U), 4U});
  }
  return digest;
  // ++++++++++++++++++++++++++++++++++++++++
}
// --------------------------------------------------------
// One compression step transforms exactly one 512-bit block into the eight-word chaining state.
void Sha256::compress(std::span<const std::byte, block_size> block) noexcept {
  // ++++++++++++++++++++++++++++++++++++++++
  // Seed the schedule from 16 encoded input words, then expand it to all 64 rounds.
  std::array<std::uint32_t, 64U> schedule{};
  for (std::size_t index = 0U; index < 16U; ++index) {
    schedule[index] =
        load_big_endian_word(std::span<const std::byte, 4U>{block.data() + (index * 4U), 4U});
  }

  for (std::size_t index = 16U; index < schedule.size(); ++index) {
    const auto sigma_zero = std::rotr(schedule[index - 15U], 7) ^
                            std::rotr(schedule[index - 15U], 18) ^ (schedule[index - 15U] >> 3U);
    const auto sigma_one = std::rotr(schedule[index - 2U], 17) ^
                           std::rotr(schedule[index - 2U], 19) ^ (schedule[index - 2U] >> 10U);
    schedule[index] = schedule[index - 16U] + sigma_zero + schedule[index - 7U] + sigma_one;
  }
  // ++++++++++++++++++++++++++++++++++++++++
  // Work on a local copy so the prior chaining state remains available for feed-forward.
  auto a = state_[0U];
  auto b = state_[1U];
  auto c = state_[2U];
  auto d = state_[3U];
  auto e = state_[4U];
  auto f = state_[5U];
  auto g = state_[6U];
  auto h = state_[7U];
  // ++++++++++++++++++++++++++++++++++++++++
  // Apply the 64 SHA-256 choice, majority, rotation, and modular-addition rounds.
  for (std::size_t index = 0U; index < schedule.size(); ++index) {
    const auto sum_one = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const auto choice = (e & f) ^ ((~e) & g);
    const auto temporary_one = h + sum_one + choice + round_constants[index] + schedule[index];
    const auto sum_zero = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const auto majority = (a & b) ^ (a & c) ^ (b & c);
    const auto temporary_two = sum_zero + majority;

    h = g;
    g = f;
    f = e;
    e = d + temporary_one;
    d = c;
    c = b;
    b = a;
    a = temporary_one + temporary_two;
  }
  // ++++++++++++++++++++++++++++++++++++++++
  // Feed the completed block back into the chaining state for the next block or final digest.
  state_[0U] += a;
  state_[1U] += b;
  state_[2U] += c;
  state_[3U] += d;
  state_[4U] += e;
  state_[5U] += f;
  state_[6U] += g;
  state_[7U] += h;
  // ++++++++++++++++++++++++++++++++++++++++
}
// --------------------------------------------------------
// Keep one-shot hashing a thin composition of the streaming contract.
Sha256Digest sha256(std::span<const std::byte> bytes) noexcept {
  Sha256 hash;
  hash.update(bytes);
  return hash.finalize();
}
// --------------------------------------------------------
// Encode each digest nibble through a fixed lowercase table, producing exactly 64 characters.
Sha256Hex sha256_hex(const Sha256Digest& digest) noexcept {
  constexpr std::array<char, 16U> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                         '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  Sha256Hex encoded{};

  for (std::size_t index = 0U; index < digest.size(); ++index) {
    const auto value = std::to_integer<unsigned int>(digest[index]);
    encoded[index * 2U] = digits[static_cast<std::size_t>(value >> 4U)];
    encoded[(index * 2U) + 1U] = digits[static_cast<std::size_t>(value & 0x0fU)];
  }
  return encoded;
}
// --------------------------------------------------------

} // namespace aegis::model
