// Cross-Cluster State Fabric - compatibility gating implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/compatibility.hpp"

#include <string>

#include "ccsf/digest.hpp"

namespace ccsf {

bool CompatibilityDescriptor::set(std::string_view key, std::string_view value) {
  if (key.empty() || key.size() > kMaxCompatibilityKeyLength) {
    return false;
  }
  if (value.size() > kMaxCompatibilityValueLength) {
    return false;
  }
  auto existing = attributes.find(key);
  if (existing != attributes.end()) {
    existing->second.assign(value.data(), value.size());
    return true;
  }
  if (attributes.size() >= kMaxCompatibilityAttributes) {
    return false;
  }
  attributes.emplace(std::string(key), std::string(value));
  return true;
}

const std::string* CompatibilityDescriptor::find(std::string_view key) const {
  auto it = attributes.find(key);
  return it == attributes.end() ? nullptr : &it->second;
}

void CompatibilityDescriptor::encode(ByteWriter& writer) const {
  writer.u32(static_cast<std::uint32_t>(attributes.size()));
  for (const auto& entry : attributes) {
    writer.str(entry.first);
    writer.str(entry.second);
  }
}

bool CompatibilityDescriptor::decode(ByteReader& reader) {
  std::uint32_t count = 0;
  if (!reader.u32(&count)) {
    return false;
  }
  if (count > kMaxCompatibilityAttributes) {
    reader.fail(ErrorCode::RESOURCE_EXHAUSTED, "compatibility attribute count exceeds the bound");
    return false;
  }
  attributes.clear();
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string key;
    std::string value;
    if (!reader.str(kMaxCompatibilityKeyLength, &key)) {
      return false;
    }
    if (!reader.str(kMaxCompatibilityValueLength, &value)) {
      return false;
    }
    if (key.empty()) {
      reader.fail(ErrorCode::INVALID_ARGUMENT, "empty compatibility attribute key");
      return false;
    }
    if (value.empty() && key.empty()) {
      reader.fail(ErrorCode::INVALID_ARGUMENT, "empty compatibility attribute");
      return false;
    }
    if (attributes.size() >= kMaxCompatibilityAttributes) {
      reader.fail(ErrorCode::RESOURCE_EXHAUSTED, "compatibility attribute count exceeds the bound");
      return false;
    }
    const bool inserted = attributes.emplace(std::move(key), std::move(value)).second;
    if (!inserted) {
      reader.fail(ErrorCode::INVALID_ARGUMENT, "duplicate compatibility attribute key");
      return false;
    }
  }
  return reader.ok();
}

std::string CompatibilityDescriptor::fingerprint() const {
  ByteWriter canonical;
  canonical.str("ccsf.compat.v1");
  encode(canonical);
  return sha256(canonical.data()).hex();
}

CompatibilityResult evaluate_compatibility(const CompatibilityRequirement& requirement,
                                           const CompatibilityDescriptor* candidate,
                                           CompatibilityGeneration candidate_generation,
                                           bool candidate_revalidation_required) noexcept {
  if (candidate == nullptr) {
    return CompatibilityResult::UNKNOWN;
  }
  if (candidate_revalidation_required) {
    return CompatibilityResult::REVALIDATION_REQUIRED;
  }
  if (candidate_generation < requirement.generation) {
    return CompatibilityResult::REVALIDATION_REQUIRED;
  }
  bool any_unknown = false;
  for (const auto& entry : requirement.required.attributes) {
    const std::string* actual = candidate->find(entry.first);
    if (actual == nullptr) {
      any_unknown = true;
      continue;
    }
    if (entry.second == CompatibilityDescriptor::kWildcard) {
      continue;
    }
    if (*actual != entry.second) {
      return CompatibilityResult::INCOMPATIBLE;
    }
  }
  if (any_unknown) {
    return CompatibilityResult::UNKNOWN;
  }
  return CompatibilityResult::COMPATIBLE;
}

CompatibilityResult evaluate_compatibility(const CompatibilityRequirement& requirement,
                                           const CompatibilityRecord* candidate) noexcept {
  if (candidate == nullptr) {
    return CompatibilityResult::UNKNOWN;
  }
  if (candidate->compatibility_id != requirement.compatibility_id) {
    return CompatibilityResult::UNKNOWN;
  }
  return evaluate_compatibility(requirement, &candidate->descriptor, candidate->generation,
                                candidate->revalidation_required);
}

std::string describe_compatibility(const CompatibilityRequirement& requirement,
                                   CompatibilityResult result,
                                   const CompatibilityDescriptor* candidate) {
  std::string text = "compatibility ";
  text += to_string(result);
  text += " for identity ";
  text += requirement.compatibility_id.str();
  text += " generation ";
  text += requirement.generation.str();
  if (candidate == nullptr) {
    text += " (no candidate evidence published)";
    return text;
  }
  text += " (candidate fingerprint ";
  text += candidate->fingerprint();
  text += ")";
  return text;
}

}  // namespace ccsf
