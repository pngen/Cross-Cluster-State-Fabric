// Cross-Cluster State Fabric - SHA-256 implementation (FIPS 180-4).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/digest.hpp"

#include <array>
#include <cstring>

namespace ccsf {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{{
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
}};

constexpr std::array<std::uint32_t, 8> kInitialState{{
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
}};

inline std::uint32_t rotr(std::uint32_t value, unsigned shift) noexcept {
  return (value >> shift) | (value << (32U - shift));
}

}  // namespace

// The hash function objects in this translation unit are deliberately free of
// reinterpret_cast on arbitrary alignment: message words are assembled bytewise.

bool Digest::is_zero() const noexcept {
  for (std::uint8_t byte : bytes) {
    if (byte != 0U) {
      return false;
    }
  }
  return true;
}

std::string Digest::hex() const {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(kSha256Length * 2U);
  for (std::uint8_t byte : bytes) {
    result.push_back(kHex[(byte >> 4U) & 0x0FU]);
    result.push_back(kHex[byte & 0x0FU]);
  }
  return result;
}

Result<Digest> Digest::from_hex(std::string_view text) {
  if (text.size() != kSha256Length * 2U) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "digest hex text must be 64 characters");
  }
  auto hex_value = [](char character) -> int {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
      return character - 'A' + 10;
    }
    return -1;
  };
  Digest digest;
  for (std::size_t index = 0; index < kSha256Length; ++index) {
    const int high = hex_value(text[index * 2U]);
    const int low = hex_value(text[index * 2U + 1U]);
    if (high < 0 || low < 0) {
      return make_error(ErrorCode::INVALID_ARGUMENT, "digest hex text contains a non-hex character");
    }
    digest.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return digest;
}

const char* to_string(IntegrityAlgorithm algorithm) noexcept {
  switch (algorithm) {
    case IntegrityAlgorithm::NONE: return "NONE";
    case IntegrityAlgorithm::SHA256: return "SHA256";
  }
  return "UNRECOGNIZED_INTEGRITY_ALGORITHM";
}

bool is_valid(IntegrityAlgorithm algorithm) noexcept {
  return algorithm == IntegrityAlgorithm::NONE || algorithm == IntegrityAlgorithm::SHA256;
}

void Sha256::reset() noexcept {
  state_ = kInitialState;
  buffer_.fill(0);
  buffered_ = 0;
  total_bytes_ = 0;
  finalized_ = false;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    const std::size_t offset = index * 4U;
    schedule[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                      (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                      (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                      static_cast<std::uint32_t>(block[offset + 3U]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15U], 7U) ^
                             rotr(schedule[index - 15U], 18U) ^
                             (schedule[index - 15U] >> 3U);
    const std::uint32_t s1 = rotr(schedule[index - 2U], 17U) ^
                             rotr(schedule[index - 2U], 19U) ^
                             (schedule[index - 2U] >> 10U);
    schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t sigma1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + sigma1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t sigma0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sigma0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::byte> data) noexcept {
  if (finalized_) {
    return;
  }
  total_bytes_ += static_cast<std::uint64_t>(data.size());
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t space = 64U - buffered_;
    const std::size_t take = (data.size() - offset < space) ? (data.size() - offset) : space;
    std::memcpy(buffer_.data() + buffered_, data.data() + offset, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == 64U) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

Digest Sha256::finalize() noexcept {
  Digest digest;
  if (finalized_) {
    return digest;
  }
  finalized_ = true;

  const std::uint64_t bit_length = total_bytes_ << 3U;

  buffer_[buffered_] = 0x80U;
  ++buffered_;

  if (buffered_ > 56U) {
    while (buffered_ < 64U) {
      buffer_[buffered_] = 0x00U;
      ++buffered_;
    }
    compress(buffer_.data());
    buffered_ = 0;
  }
  while (buffered_ < 56U) {
    buffer_[buffered_] = 0x00U;
    ++buffered_;
  }
  for (unsigned index = 0; index < 8U; ++index) {
    buffer_[56U + index] = static_cast<std::uint8_t>(
        (bit_length >> (56U - 8U * index)) & 0xFFU);
  }
  compress(buffer_.data());
  buffered_ = 0;

  for (std::size_t index = 0; index < 8U; ++index) {
    digest.bytes[index * 4U] = static_cast<std::uint8_t>((state_[index] >> 24U) & 0xFFU);
    digest.bytes[index * 4U + 1U] = static_cast<std::uint8_t>((state_[index] >> 16U) & 0xFFU);
    digest.bytes[index * 4U + 2U] = static_cast<std::uint8_t>((state_[index] >> 8U) & 0xFFU);
    digest.bytes[index * 4U + 3U] = static_cast<std::uint8_t>(state_[index] & 0xFFU);
  }
  return digest;
}

Digest sha256(std::span<const std::byte> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finalize();
}

Digest sha256(std::string_view text) noexcept {
  return sha256(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                           text.size()));
}

}  // namespace ccsf
