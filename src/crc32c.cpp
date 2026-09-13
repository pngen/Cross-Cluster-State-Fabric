// Cross-Cluster State Fabric - CRC-32C implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/crc32c.hpp"

#include <array>

namespace ccsf {
namespace {

constexpr std::uint32_t kPolynomial = 0x82F63B78U;

using Table = std::array<std::uint32_t, 256>;

constexpr Table make_table() noexcept {
  Table table{};
  for (std::uint32_t index = 0; index < 256U; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1U) != 0U ? ((value >> 1U) ^ kPolynomial) : (value >> 1U);
    }
    table[index] = value;
  }
  return table;
}

constexpr Table kTable = make_table();

constexpr std::uint32_t kInitial = 0xFFFFFFFFU;
constexpr std::uint32_t kFinalXor = 0xFFFFFFFFU;

}  // namespace

std::uint32_t crc32c_extend(std::uint32_t previous, std::span<const std::byte> data) noexcept {
  std::uint32_t crc = previous ^ kInitial;
  for (const std::byte raw : data) {
    const auto index = static_cast<std::uint32_t>(
        (crc ^ std::to_integer<std::uint8_t>(raw)) & 0xFFU);
    crc = kTable[index] ^ (crc >> 8U);
  }
  return crc ^ kInitial;
}

std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  return crc32c_extend(0U, data);
}

}  // namespace ccsf
