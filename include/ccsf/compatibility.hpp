// Cross-Cluster State Fabric - compatibility gating.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Compatibility is a hard gate, not a ranking factor. Unknown compatibility is
// never treated as compatible. The runtime consumes externally produced
// compatibility identities and provides a narrow local descriptor interface so
// that it can operate standalone without duplicating a compatibility registry.

#ifndef CCSF_COMPATIBILITY_HPP
#define CCSF_COMPATIBILITY_HPP

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "ccsf/bytes.hpp"
#include "ccsf/clock.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/taxonomy.hpp"

namespace ccsf {

/// Upper bounds that keep untrusted metadata bounded before allocation.
inline constexpr std::size_t kMaxCompatibilityAttributes = 256;
inline constexpr std::size_t kMaxCompatibilityKeyLength = 128;
inline constexpr std::size_t kMaxCompatibilityValueLength = 512;

/// A canonical, ordered set of compatibility attributes.
///
/// Keys are compared as raw byte strings; iteration order is lexicographic so
/// that serialization and fingerprints are canonical for equal content.
struct CompatibilityDescriptor {
  std::map<std::string, std::string, std::less<>> attributes;

  /// Wildcard value: matches any present candidate value.
  static constexpr std::string_view kWildcard = "*";

  [[nodiscard]] bool empty() const noexcept { return attributes.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return attributes.size(); }

  /// Inserts or replaces an attribute. Returns false when a bound was exceeded.
  bool set(std::string_view key, std::string_view value);

  [[nodiscard]] const std::string* find(std::string_view key) const;

  /// SHA-256 over the canonical encoding, rendered as hex.
  [[nodiscard]] std::string fingerprint() const;

  void encode(ByteWriter& writer) const;
  bool decode(ByteReader& reader);

  friend bool operator==(const CompatibilityDescriptor&, const CompatibilityDescriptor&) = default;
};

/// The compatibility identity a state generation requires of any replica.
struct CompatibilityRequirement {
  CompatibilityId compatibility_id;
  CompatibilityGeneration generation;
  CompatibilityDescriptor required;
  bool require_verified_evidence{true};

  friend bool operator==(const CompatibilityRequirement&,
                         const CompatibilityRequirement&) = default;
};

/// A published compatibility identity for a producer (a cluster, a runtime, a
/// model build). This is the fabric's local compatibility record, not a full
/// compatibility registry.
struct CompatibilityRecord {
  CompatibilityId compatibility_id;
  CompatibilityGeneration generation;
  CompatibilityDescriptor descriptor;
  std::string producer;
  Tick published_at{0};
  bool revalidation_required{false};
};

/// Evaluates a candidate descriptor against a requirement.
///
/// - a missing candidate yields UNKNOWN;
/// - a candidate whose evidence generation is behind the requirement, or which
///   is flagged for revalidation, yields REVALIDATION_REQUIRED;
/// - a required attribute the candidate does not carry yields UNKNOWN;
/// - a required attribute with a concrete, differing value yields INCOMPATIBLE;
/// - only a fully satisfied requirement yields COMPATIBLE.
CompatibilityResult evaluate_compatibility(const CompatibilityRequirement& requirement,
                                           const CompatibilityDescriptor* candidate,
                                           CompatibilityGeneration candidate_generation,
                                           bool candidate_revalidation_required) noexcept;

/// Convenience overload evaluating against a published record.
CompatibilityResult evaluate_compatibility(const CompatibilityRequirement& requirement,
                                           const CompatibilityRecord* candidate) noexcept;

/// Explains a compatibility result in one deterministic line.
std::string describe_compatibility(const CompatibilityRequirement& requirement,
                                   CompatibilityResult result,
                                   const CompatibilityDescriptor* candidate);

}  // namespace ccsf

#endif  // CCSF_COMPATIBILITY_HPP
