// Cross-Cluster State Fabric - economic quantity implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/cost.hpp"

#include <array>
#include <limits>
#include <utility>

namespace ccsf {
namespace {

constexpr std::array<std::pair<CostSource, const char*>, 4> kCostSources{{
    {CostSource::UNKNOWN, "UNKNOWN"},
    {CostSource::MEASURED, "MEASURED"},
    {CostSource::POLICY, "POLICY"},
    {CostSource::ESTIMATED, "ESTIMATED"},
}};

/// Weaker evidence has a lower rank; combining keeps the weaker provenance.
constexpr int source_rank(CostSource source) noexcept {
  switch (source) {
    case CostSource::UNKNOWN: return 0;
    case CostSource::ESTIMATED: return 1;
    case CostSource::POLICY: return 2;
    case CostSource::MEASURED: return 3;
  }
  return 0;
}

}  // namespace

const char* to_string(CostSource source) noexcept {
  for (const auto& entry : kCostSources) {
    if (entry.first == source) {
      return entry.second;
    }
  }
  return "UNRECOGNIZED_COST_SOURCE";
}

bool is_valid(CostSource source) noexcept {
  for (const auto& entry : kCostSources) {
    if (entry.first == source) {
      return true;
    }
  }
  return false;
}

std::int64_t saturating_add(std::int64_t a, std::int64_t b) noexcept {
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return a + b;
}

std::int64_t saturating_mul(std::int64_t a, std::int64_t b) noexcept {
  if (a == 0 || b == 0) {
    return 0;
  }
  if (a == -1 && b == std::numeric_limits<std::int64_t>::min()) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (b == -1 && a == std::numeric_limits<std::int64_t>::min()) {
    return std::numeric_limits<std::int64_t>::max();
  }
  const std::int64_t product = a * b;
  if (product / b != a) {
    return (a > 0) == (b > 0) ? std::numeric_limits<std::int64_t>::max()
                              : std::numeric_limits<std::int64_t>::min();
  }
  return product;
}

std::string describe(const CostValue& value, const std::string& unit_label) {
  if (!value.known()) {
    return std::string("UNKNOWN ") + unit_label;
  }
  const bool negative = value.micro_units < 0;
  const std::uint64_t magnitude =
      negative ? static_cast<std::uint64_t>(-(value.micro_units + 1)) + 1U
               : static_cast<std::uint64_t>(value.micro_units);
  const std::uint64_t whole = magnitude / static_cast<std::uint64_t>(kMicroUnitsPerUnit);
  const std::uint64_t fraction = magnitude % static_cast<std::uint64_t>(kMicroUnitsPerUnit);

  std::string result = to_string(value.source);
  result += ' ';
  if (negative) {
    result += '-';
  }
  result += std::to_string(whole);
  result += '.';
  std::string digits = std::to_string(fraction);
  result.append(6U - digits.size(), '0');
  result += digits;
  result += ' ';
  result += unit_label;
  return result;
}

CostValue cost_add(const CostValue& a, const CostValue& b) noexcept {
  if (!a.known() || !b.known()) {
    return CostValue::unknown();
  }
  const CostSource combined =
      source_rank(a.source) <= source_rank(b.source) ? a.source : b.source;
  return CostValue{combined, saturating_add(a.micro_units, b.micro_units)};
}

CostValue cost_sum(const CostValue& a, const CostValue& b) noexcept {
  const bool present_a = a.known() || a.micro_units != 0;
  const bool present_b = b.known() || b.micro_units != 0;
  if (!present_a) {
    return b;
  }
  if (!present_b) {
    return a;
  }
  return cost_add(a, b);
}

CostValue cost_scale(const CostValue& value, std::uint64_t numerator,
                     std::uint64_t denominator) noexcept {
  if (!value.known()) {
    return CostValue::unknown();
  }
  if (denominator == 0) {
    return CostValue::unknown();
  }
  if (numerator == 0) {
    return CostValue{value.source, 0};
  }
  const std::uint64_t limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (numerator != 0 && static_cast<std::uint64_t>(value.micro_units < 0 ? -value.micro_units
                                                                        : value.micro_units) >
                            limit / numerator) {
    return CostValue::unknown();
  }
  const std::int64_t scaled = value.micro_units * static_cast<std::int64_t>(numerator);
  return CostValue{value.source, scaled / static_cast<std::int64_t>(denominator)};
}

int cost_compare(const CostValue& a, const CostValue& b) noexcept {
  if (!a.known() && !b.known()) {
    return 0;
  }
  if (!a.known()) {
    return 1;
  }
  if (!b.known()) {
    return -1;
  }
  if (a.micro_units < b.micro_units) {
    return -1;
  }
  if (a.micro_units > b.micro_units) {
    return 1;
  }
  // Equal magnitude: stronger evidence wins the tie deterministically.
  const int rank_a = source_rank(a.source);
  const int rank_b = source_rank(b.source);
  if (rank_a == rank_b) {
    return 0;
  }
  return rank_a > rank_b ? -1 : 1;
}

std::string describe(const SizeValue& value) {
  if (!value.known()) {
    return "UNKNOWN bytes";
  }
  return std::to_string(value.bytes) + " bytes";
}

std::uint64_t ReuseValue::expected_value_milli() const noexcept {
  const std::uint64_t count = expected_reuse_count;
  const std::uint64_t probability = probability_per_mille;
  if (count != 0 && probability != 0 &&
      count > std::numeric_limits<std::uint64_t>::max() / probability) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return count * probability;
}

std::string describe(const ReuseValue& value) {
  if (!value.known()) {
    return "UNKNOWN expected reuse";
  }
  std::string result = to_string(value.source);
  result += " expected reuse ";
  result += std::to_string(value.expected_reuse_count);
  result += " at ";
  const std::uint64_t whole = value.probability_per_mille / 10U;
  const std::uint64_t fraction = value.probability_per_mille % 10U;
  result += std::to_string(whole);
  result += '.';
  result += std::to_string(fraction);
  result += " probability";
  return result;
}

}  // namespace ccsf
