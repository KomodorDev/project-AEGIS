// Purpose: verify portable SHA-256 hashing against published standard vectors.

#include "aegis/model/sha256.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) noexcept {
  return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

[[nodiscard]] std::string hex_of(const aegis::model::Sha256Digest& digest) {
  const auto encoded = aegis::model::sha256_hex(digest);
  return {encoded.begin(), encoded.end()};
}

[[nodiscard]] std::string hash_text(std::string_view text) {
  return hex_of(aegis::model::sha256(as_bytes(text)));
}

} // namespace

TEST_CASE("SHA-256 matches the empty input standard vector", "[model][sha256]") {
  CHECK(hash_text("") == "e3b0c44298fc1c149afbf4c8996fb924"
                         "27ae41e4649b934ca495991b7852b855");
}

TEST_CASE("SHA-256 matches the abc standard vector", "[model][sha256]") {
  CHECK(hash_text("abc") == "ba7816bf8f01cfea414140de5dae2223"
                            "b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("SHA-256 matches the million-a multi-block standard vector", "[model][sha256]") {
  const std::string million_as(1'000'000U, 'a');
  CHECK(hash_text(million_as) == "cdc76e5c9914fb9281a1c7e284d73e67"
                                 "f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("incremental SHA-256 is independent of chunk boundaries", "[model][sha256]") {
  constexpr std::string_view input = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                                     "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
  constexpr std::array<std::size_t, 4U> chunk_sizes{1U, 63U, 2U, 46U};

  aegis::model::Sha256 incremental;
  std::size_t consumed = 0U;
  for (const auto chunk_size : chunk_sizes) {
    incremental.update(as_bytes(input.substr(consumed, chunk_size)));
    consumed += chunk_size;
  }

  REQUIRE(consumed == input.size());
  CHECK(incremental.finalize() == aegis::model::sha256(as_bytes(input)));
  CHECK(hex_of(incremental.finalize()) == "cf5b16a778af8380036ce59e7b049237"
                                          "0b249b11e8f07a51afac45037afee9d1");
}

TEST_CASE("SHA-256 hexadecimal encoding is lowercase and fixed width", "[model][sha256]") {
  aegis::model::Sha256Digest digest{};
  for (std::size_t index = 0U; index < digest.size(); ++index) {
    digest[index] = static_cast<std::byte>(index);
  }

  const auto encoded = aegis::model::sha256_hex(digest);
  CHECK(encoded.size() == aegis::model::sha256_hex_size);
  CHECK(std::string(encoded.begin(), encoded.end()) == "000102030405060708090a0b0c0d0e0f"
                                                       "101112131415161718191a1b1c1d1e1f");
}

TEST_CASE("finalizing a SHA-256 prefix does not consume incremental state", "[model][sha256]") {
  aegis::model::Sha256 incremental;
  incremental.update(as_bytes("a"));
  CHECK(incremental.finalize() == aegis::model::sha256(as_bytes("a")));

  incremental.update(as_bytes("bc"));
  CHECK(incremental.finalize() == aegis::model::sha256(as_bytes("abc")));
}
