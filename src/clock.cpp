// Cross-Cluster State Fabric - wall clock rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/clock.hpp"

#include <ctime>
#include <cstdio>

namespace ccsf {

std::string format_wall_clock_utc() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#if defined(_WIN32)
  if (gmtime_s(&utc, &seconds) != 0) {
    return "1970-01-01T00:00:00Z";
  }
#else
  if (gmtime_r(&seconds, &utc) == nullptr) {
    return "1970-01-01T00:00:00Z";
  }
#endif
  char buffer[32] = {};
  const std::size_t written =
      std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  if (written == 0) {
    return "1970-01-01T00:00:00Z";
  }
  return std::string(buffer, written);
}

}  // namespace ccsf
