// Cross-Cluster State Fabric - content digests and SHA-256.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SHA-256 is implemented in-tree so that integrity verification has no external
// dependency and so that padding boundaries are covered by first-party tests.
// The implementation is a straightforward FIPS 180-4 construction; it is used
// for content integrity, not for authentication.

#ifndef CCSF_DIGEST_HPP
#define CCSF_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "ccsf/result.hpp"

namespace ccsf {

inline constexpr std::size_t kSha256Length = 32;

/// A 256-bit content digest. The all-zero digest means "unknown/absent".
struct Digest {
  std::array<std::uint8_t, kSha256Length> bytes{};

  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] bool valid() const noexcept { return !is_zero(); }

  /// Lower-case hexadecimal rendering, 64 characters.
  [[nodiscard]] std::string hex() const;

  /// Parses 64 hexadecimal characters (either case).
  static Result<Digest> from_hex(std::string_view text);

  friend bool operator==(const Digest&, const Digest&) noexcept = default;
  friend bool operator<(const Digest& a, const Digest& b) noexcept {
    return a.bytes < b.bytes;
  }
};

/// Names of the integrity algorithms this build understands. Algorithm agility
/// is explicit so a downgrade cannot be silent.
enum class IntegrityAlgorithm : std::uint16_t {
  NONE = 0,
  SHA256 = 1,
};

const char* to_string(IntegrityAlgorithm algorithm) noexcept;
bool is_valid(IntegrityAlgorithm algorithm) noexcept;

/// Streaming SHA-256 state.
class Sha256 {
 public:
  Sha256() { reset(); }

  void reset() noexcept;
  void update(std::span<const std::byte> data) noexcept;
  void update(std::string_view text) noexcept;
  /// Finalizes and returns the digest. The object must be reset before reuse.
  Digest finalize() noexcept;

  /// Number of bytes absorbed so far.
  [[nodiscard]] std::uint64_t byte_count() const noexcept { return total_bytes_; }

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_{0};
  std::uint64_t total_bytes_{0};
  bool finalized_{false};
};

/// One-shot SHA-256 over an arbitrary byte range.
Digest sha256(std::span<const std::byte> data) noexcept;
Digest sha256(std::string_view text) noexcept;

}  // namespace ccsf

namespace std {
template <>
struct hash<ccsf::Digest> {
  size_t operator()(const ccsf::Digest& digest) const noexcept {
    size_t seed = 1469598103934665603ULL;
    for (std::uint8_t byte : digest.bytes) {
      seed ^= static_cast<size_t>(byte);
      seed *= 1099511628211ULL;
    }
    return seed;
  }
};
}  // namespace std

#endif  // CCSF_DIGEST_HPP
