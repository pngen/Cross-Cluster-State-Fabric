// Cross-Cluster State Fabric - economic quantities.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Economic values carry their provenance. UNKNOWN is a distinct state from
// zero: an absent price is not a free price, and arithmetic on UNKNOWN
// propagates UNKNOWN rather than collapsing to 0.
//
// All monetary arithmetic is exact integer arithmetic in micro-units
// (1 unit = 1,000,000 micro-units). No floating point value can influence a
// governed decision.

#ifndef CCSF_COST_HPP
#define CCSF_COST_HPP

#include <cstdint>
#include <string>

#include <optional>

namespace ccsf {

/// Provenance of an economic or quantitative value.
enum class CostSource : std::uint16_t {
  UNKNOWN = 0,
  MEASURED = 1,
  POLICY = 2,
  ESTIMATED = 3
};

const char* to_string(CostSource source) noexcept;
bool is_valid(CostSource source) noexcept;

inline constexpr std::int64_t kMicroUnitsPerUnit = 1000000;

/// A quantity of cost in micro-units together with its provenance.
struct CostValue {
  CostSource source{CostSource::UNKNOWN};
  std::int64_t micro_units{0};

  [[nodiscard]] bool known() const noexcept { return source != CostSource::UNKNOWN; }

  static CostValue unknown() noexcept { return CostValue{CostSource::UNKNOWN, 0}; }
  static CostValue measured(std::int64_t micro_units) noexcept {
    return CostValue{CostSource::MEASURED, micro_units};
  }
  static CostValue from_policy(std::int64_t micro_units) noexcept {
    return CostValue{CostSource::POLICY, micro_units};
  }
  static CostValue estimated(std::int64_t micro_units) noexcept {
    return CostValue{CostSource::ESTIMATED, micro_units};
  }

  friend bool operator==(const CostValue&, const CostValue&) = default;
};

/// Renders like "POLICY 0.180000 units" or "UNKNOWN units".
std::string describe(const CostValue& value, const std::string& unit_label = "units");

/// Adds two costs. UNKNOWN absorbs; the combined provenance is the weaker one.
CostValue cost_add(const CostValue& a, const CostValue& b) noexcept;

/// Sums cost components. A component that carries neither a known provenance nor
/// a magnitude is treated as absent (not applicable). A component that is present
/// but unpriced poisons the total to UNKNOWN, so an absent price is never zero.
CostValue cost_sum(const CostValue& a, const CostValue& b) noexcept;

/// Scales a cost by numerator/denominator. Overflow and UNKNOWN yield UNKNOWN.
CostValue cost_scale(const CostValue& value, std::uint64_t numerator,
                     std::uint64_t denominator) noexcept;

/// Orders two costs, treating UNKNOWN as strictly worse (larger) than anything
/// known. Returns <0 when a is cheaper, 0 when equal, >0 when a is worse.
int cost_compare(const CostValue& a, const CostValue& b) noexcept;

/// A size in bytes that may be unknown. Unknown is not zero.
struct SizeValue {
  bool present{false};
  std::uint64_t bytes{0};

  [[nodiscard]] bool known() const noexcept { return present; }
  static SizeValue unknown() noexcept { return SizeValue{}; }
  static SizeValue of(std::uint64_t bytes) noexcept { return SizeValue{true, bytes}; }

  friend bool operator==(const SizeValue&, const SizeValue&) = default;
};

std::string describe(const SizeValue& value);

/// Expected reuse value of a state generation. Unknown reuse value never wins a
/// ranking comparison against a known value.
struct ReuseValue {
  CostSource source{CostSource::UNKNOWN};
  std::uint64_t expected_reuse_count{0};
  std::uint32_t probability_per_mille{0};

  [[nodiscard]] bool known() const noexcept { return source != CostSource::UNKNOWN; }

  /// Deterministic integer expected value: count * probability (in thousandths).
  [[nodiscard]] std::uint64_t expected_value_milli() const noexcept;

  static ReuseValue unknown() noexcept { return ReuseValue{}; }
  static ReuseValue measured(std::uint64_t count, std::uint32_t probability_per_mille) noexcept {
    return ReuseValue{CostSource::MEASURED, count, probability_per_mille};
  }
  static ReuseValue estimated(std::uint64_t count, std::uint32_t probability_per_mille) noexcept {
    return ReuseValue{CostSource::ESTIMATED, count, probability_per_mille};
  }

  friend bool operator==(const ReuseValue&, const ReuseValue&) = default;
};

std::string describe(const ReuseValue& value);

/// Saturating 64-bit helpers used by the deterministic scoring path.
std::int64_t saturating_add(std::int64_t a, std::int64_t b) noexcept;
std::int64_t saturating_mul(std::int64_t a, std::int64_t b) noexcept;

}  // namespace ccsf

#endif  // CCSF_COST_HPP
