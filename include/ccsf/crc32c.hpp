// Cross-Cluster State Fabric - CRC-32C (Castagnoli) frame integrity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CCSF_CRC32C_HPP
#define CCSF_CRC32C_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace ccsf {

/// CRC-32C (polynomial 0x1EDC6F41, reflected 0x82F63B78), init 0xFFFFFFFF,
/// final XOR 0xFFFFFFFF. Check value: crc32c("123456789") == 0xE3069283.
std::uint32_t crc32c(std::span<const std::byte> data) noexcept;

/// Continues a CRC-32C computation over a second range, where the argument
/// "previous" is the value returned by an earlier call on the preceding range.
std::uint32_t crc32c_extend(std::uint32_t previous, std::span<const std::byte> data) noexcept;

}  // namespace ccsf

#endif  // CCSF_CRC32C_HPP
