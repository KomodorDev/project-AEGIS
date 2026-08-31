// Purpose: verify portable SHA-256 hashing against published standard vectors.

#include "aegis/model/sha256.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace {

// --------------------------------------------------------
// Interesting syntax: std::as_bytes creates a read-only byte view over text without copying or
// applying an encoding conversion, matching the production hash boundary exactly.
[[nodiscard]] std::span<const std::byte> text_as_bytes(std::string_view text) noexcept {
  return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

// --------------------------------------------------------
// Render a digest as an owning string so assertions can compare published vectors directly.
[[nodiscard]] std::string digest_to_hex(const aegis::model::Sha256Digest& digest) {
  const auto encoded = aegis::model::sha256_hex_from_digest(digest);
  return {encoded.begin(), encoded.end()};
}

// --------------------------------------------------------
// Hash textual bytes through the same one-shot boundary used by provenance callers.
[[nodiscard]] std::string calculate_sha256_hex(std::string_view text) {
  return digest_to_hex(aegis::model::calculate_sha256_digest(text_as_bytes(text)));
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Published short-message vectors pin empty padding and the ordinary single-block transform.
TEST_CASE("SHA-256 matches the empty input standard vector", "[model][sha256]") {
  CHECK(calculate_sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb924"
                                    "27ae41e4649b934ca495991b7852b855");
}

// --------------------------------------------------------
// The canonical "abc" vector catches errors in message loading, rounds, and digest serialization.
TEST_CASE("SHA-256 matches the abc standard vector", "[model][sha256]") {
  CHECK(calculate_sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223"
                                       "b00361a396177a9cb410ff61f20015ad");
}

// --------------------------------------------------------
// The million-byte published vector exercises repeated direct-block compression and final padding.
TEST_CASE("SHA-256 matches the million-a multi-block standard vector", "[model][sha256]") {
  const std::string million_as(1'000'000U, 'a');
  CHECK(calculate_sha256_hex(million_as) == "cdc76e5c9914fb9281a1c7e284d73e67"
                                            "f1809a48a497200e046d39ccc7112cd0");
}

// --------------------------------------------------------
// Deliberately uneven chunks cross the 64-byte boundary and prove buffering does not affect
// identity.
TEST_CASE("incremental SHA-256 is independent of chunk boundaries", "[model][sha256]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Define a multi-block message and chunks that deliberately straddle block boundaries.
  constexpr std::string_view input = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                                     "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
  constexpr std::array<std::size_t, 4U> chunk_sizes{1U, 63U, 2U, 46U};

  // ++++++++++++++++++++++++++++++++++++++++
  // Feed every chunk through the stateful interface while tracking complete consumption.
  aegis::model::Sha256 incremental;
  std::size_t consumed = 0U;
  for (const auto chunk_size : chunk_sizes) {
    incremental.append_bytes(text_as_bytes(input.substr(consumed, chunk_size)));
    consumed += chunk_size;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove chunking matches both the one-shot implementation and the published vector.
  REQUIRE(consumed == input.size());
  CHECK(incremental.derive_digest() == aegis::model::calculate_sha256_digest(text_as_bytes(input)));
  CHECK(digest_to_hex(incremental.derive_digest()) == "cf5b16a778af8380036ce59e7b049237"
                                                      "0b249b11e8f07a51afac45037afee9d1");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Provenance rendering is a fixed-width lowercase array, including leading-zero nibbles.
TEST_CASE("SHA-256 hexadecimal encoding is lowercase and fixed width", "[model][sha256]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Populate all byte values needed to expose leading-zero and alphabetic nibble formatting.
  aegis::model::Sha256Digest digest{};
  for (std::size_t index = 0U; index < digest.size(); ++index) {
    digest[index] = static_cast<std::byte>(index);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Verify both the fixed extent and the exact lowercase representation.
  const auto encoded = aegis::model::sha256_hex_from_digest(digest);
  CHECK(encoded.size() == aegis::model::sha256_hex_size);
  CHECK(std::string(encoded.begin(), encoded.end()) == "000102030405060708090a0b0c0d0e0f"
                                                       "101112131415161718191a1b1c1d1e1f");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Finalization is an observational snapshot: hashing may continue from the same prefix afterward.
TEST_CASE("finalizing a SHA-256 prefix does not consume incremental state", "[model][sha256]") {
  aegis::model::Sha256 incremental;
  incremental.append_bytes(text_as_bytes("a"));
  CHECK(incremental.derive_digest() == aegis::model::calculate_sha256_digest(text_as_bytes("a")));

  incremental.append_bytes(text_as_bytes("bc"));
  CHECK(incremental.derive_digest() == aegis::model::calculate_sha256_digest(text_as_bytes("abc")));
}

// --------------------------------------------------------
