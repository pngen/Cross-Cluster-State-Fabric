// Cross-Cluster State Fabric - version implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/version.hpp"

namespace ccsf {

const char* version_string() noexcept { return CCSF_VERSION_STRING; }

std::uint32_t version_packed() noexcept {
  return (static_cast<std::uint32_t>(kVersionMajor) << 16U) |
         (static_cast<std::uint32_t>(kVersionMinor) << 8U) |
         static_cast<std::uint32_t>(kVersionPatch);
}

}  // namespace ccsf
