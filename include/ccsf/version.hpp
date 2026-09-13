// Cross-Cluster State Fabric - version information.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CCSF_VERSION_HPP
#define CCSF_VERSION_HPP

#include <cstdint>
#include <string>

#define CCSF_VERSION_MAJOR 1
#define CCSF_VERSION_MINOR 0
#define CCSF_VERSION_PATCH 0
#define CCSF_VERSION_STRING "1.0.0"

namespace ccsf {

inline constexpr std::uint32_t kVersionMajor = CCSF_VERSION_MAJOR;
inline constexpr std::uint32_t kVersionMinor = CCSF_VERSION_MINOR;
inline constexpr std::uint32_t kVersionPatch = CCSF_VERSION_PATCH;

/// Human readable semantic version of the linked runtime, e.g. "1.0.0".
const char* version_string() noexcept;

/// Packed 32-bit version, major << 16 | minor << 8 | patch.
std::uint32_t version_packed() noexcept;

}  // namespace ccsf

#endif  // CCSF_VERSION_HPP
