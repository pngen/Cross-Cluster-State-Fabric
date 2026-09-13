// Cross-Cluster State Fabric - deterministic logical time.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every governed decision in this runtime is made against a coordinator-owned
// logical tick counter, never against wall-clock time. Wall-clock rendering is
// for human consumption only and is never an input to a decision. This keeps
// decisions reproducible from the durable control plane alone.

#ifndef CCSF_CLOCK_HPP
#define CCSF_CLOCK_HPP

#include <chrono>
#include <cstdint>
#include <string>

namespace ccsf {

using Tick = std::uint64_t;

/// Coordinator-owned monotonic logical clock. Ticks never go backwards.
class LogicalClock {
 public:
  constexpr LogicalClock() noexcept = default;
  explicit constexpr LogicalClock(Tick start) noexcept : now_(start) {}

  [[nodiscard]] constexpr Tick now() const noexcept { return now_; }

  /// Advances by "delta" (default 1) and returns the new value.
  constexpr Tick advance(Tick delta = 1) noexcept {
    now_ += delta;
    return now_;
  }

  /// Moves the clock forward to "value" if it is ahead; never moves backwards.
  constexpr void observe(Tick value) noexcept {
    if (value > now_) {
      now_ = value;
    }
  }

  /// Restores an exact value; used by durable replay only.
  constexpr void restore(Tick value) noexcept { now_ = value; }

 private:
  Tick now_{0};
};

/// Wall-clock rendering helper (reporting only, never decision input).
std::string format_wall_clock_utc();

}  // namespace ccsf

#endif  // CCSF_CLOCK_HPP
