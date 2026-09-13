// Cross-Cluster State Fabric - deterministic placement evaluation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Hard constraints are evaluated before any score exists. Ranking then uses
// exact integer fixed-point arithmetic over named factors, with pessimistic
// treatment of absent evidence: an UNKNOWN factor is scored as the worst value
// the policy could have preferred, so missing evidence can never look best.
// Tie-breaking is total: score, then diversity, then region affinity, then the
// cluster identity. Equal canonical inputs always produce equal decisions.

#include "ccsf/placement.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace ccsf {
namespace {

constexpr std::int64_t kFixedOne = 1000000;

/// Absolute normalization caps. They are part of the documented decision model:
/// they never depend on which other candidates happen to be present.
constexpr std::uint64_t kReuseCapMilli = 1000000ULL;
constexpr std::int64_t kCostCapMicro = 100000000;          // 100.000000 units
constexpr std::uint64_t kBandwidthCapBytesPerTick = 10ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kDurationCapTicks = 1000000ULL;

std::int64_t normalize_u64(std::uint64_t value, std::uint64_t cap) noexcept {
  if (cap == 0U) {
    return 0;
  }
  if (value >= cap) {
    return kFixedOne;
  }
  const std::uint64_t scaled = (value * static_cast<std::uint64_t>(kFixedOne)) / cap;
  return static_cast<std::int64_t>(scaled);
}

std::int64_t normalize_cost(const CostValue& value) noexcept {
  if (!value.known()) {
    return 0;
  }
  const std::int64_t magnitude = value.micro_units < 0 ? -value.micro_units : value.micro_units;
  if (magnitude >= kCostCapMicro) {
    return kFixedOne;
  }
  return (magnitude * kFixedOne) / kCostCapMicro;
}

std::int64_t health_score(ClusterHealthState health) noexcept {
  switch (health) {
    case ClusterHealthState::HEALTHY: return kFixedOne;
    case ClusterHealthState::DEGRADED: return kFixedOne / 2;
    case ClusterHealthState::DRAINING: return kFixedOne / 4;
    case ClusterHealthState::UNKNOWN:
    case ClusterHealthState::UNREACHABLE:
    case ClusterHealthState::FAILED: return 0;
  }
  return 0;
}

bool contains_cluster(const std::vector<ClusterId>& values, ClusterId value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool contains_region(const std::vector<RegionId>& values, RegionId value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool contains_domain(const std::vector<FailureDomainId>& values, FailureDomainId value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::int64_t diversity_score(bool already_present, std::uint32_t present_count,
                             std::uint32_t required) noexcept {
  if (!already_present) {
    return kFixedOne;
  }
  if (required == 0) {
    return kFixedOne / 4;
  }
  return present_count >= required ? kFixedOne / 4 : 0;
}

struct FactorInput {
  RankingFactor factor;
  bool unknown;
  std::int64_t normalized;
  std::string evidence;
};

}  // namespace

ConstraintOutcome evaluate_hard_constraints(const PlacementCandidate& candidate,
                                            const PlacementRequest& request,
                                            const StateGenerationRecord* state) {
  ConstraintOutcome outcome;
  const PlacementConstraints& constraints = request.policy.constraints;
  const EligibilityRules& destination_rules = request.replication_policy.destination_rules;

  auto reject = [&outcome](const char* constraint, std::string detail, ErrorCode code) {
    outcome.accepted = false;
    outcome.constraint = constraint;
    outcome.detail = std::move(detail);
    outcome.code = code;
    return outcome;
  };

  if (state == nullptr) {
    return reject("state_known", "no governed state generation record is available",
                  ErrorCode::UNKNOWN_STATE);
  }
  if (!candidate.cluster_id.valid()) {
    return reject("cluster_identity", "candidate has no cluster identity",
                  ErrorCode::INVALID_ARGUMENT);
  }
  if (!candidate.cluster_incarnation.valid()) {
    return reject("cluster_incarnation",
                  "candidate cluster has no live incarnation for " + candidate.cluster_id.str(),
                  ErrorCode::STALE_CLUSTER_INCARNATION);
  }
  if (contains_cluster(constraints.prohibited_clusters, candidate.cluster_id)) {
    return reject("prohibited_cluster",
                  "cluster " + candidate.cluster_id.str() + " is prohibited by policy",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  if (!constraints.legal_clusters.empty() &&
      !contains_cluster(constraints.legal_clusters, candidate.cluster_id)) {
    return reject("legal_cluster",
                  "cluster " + candidate.cluster_id.str() + " is not in the legal cluster set",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  if (!constraints.legal_regions.empty() &&
      !contains_region(constraints.legal_regions, candidate.region)) {
    return reject("legal_region",
                  "region " + candidate.region.str() + " is not in the legal region set",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  if (contains_region(constraints.prohibited_regions, candidate.region)) {
    return reject("prohibited_region",
                  "region " + candidate.region.str() + " is prohibited by policy",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  if (constraints.require_locality &&
      !contains_cluster(constraints.locality_clusters, candidate.cluster_id)) {
    return reject("required_locality",
                  "cluster " + candidate.cluster_id.str() + " is outside the required locality",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  if (contains_cluster(destination_rules.denied_clusters, candidate.cluster_id)) {
    return reject("destination_denied",
                  "cluster " + candidate.cluster_id.str() + " is denied by replication policy",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  if (!destination_rules.allowed_clusters.empty() &&
      !contains_cluster(destination_rules.allowed_clusters, candidate.cluster_id)) {
    return reject("destination_not_allowed",
                  "cluster " + candidate.cluster_id.str() + " is not an allowed destination",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  if (candidate.already_hosts_current_replica && !destination_rules.allow_same_cluster) {
    return reject("duplicate_destination",
                  "cluster " + candidate.cluster_id.str() +
                      " already hosts a current replica of this generation",
                  ErrorCode::DUPLICATE_CONFLICT);
  }
  if (destination_rules.require_healthy_destination) {
    if (candidate.health == ClusterHealthState::UNREACHABLE ||
        candidate.health == ClusterHealthState::FAILED ||
        candidate.health == ClusterHealthState::UNKNOWN) {
      return reject("destination_health",
                    std::string("cluster ") + candidate.cluster_id.str() + " health is " +
                        to_string(candidate.health),
                    ErrorCode::DESTINATION_INELIGIBLE);
    }
  }
  if (constraints.required_health == ClusterHealthState::HEALTHY &&
      candidate.health != ClusterHealthState::HEALTHY) {
    return reject("required_health",
                  std::string("cluster ") + candidate.cluster_id.str() +
                      " does not meet the required health floor",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  for (const std::string& capability : constraints.required_capabilities) {
    if (!candidate.capabilities.has_tag(capability)) {
      return reject("required_capability",
                    "cluster " + candidate.cluster_id.str() + " lacks capability " + capability,
                    ErrorCode::DESTINATION_INELIGIBLE);
    }
  }
  for (const std::string& storage_class : constraints.required_storage_classes) {
    if (!candidate.capabilities.has_storage_class(storage_class)) {
      return reject("required_storage_class",
                    "cluster " + candidate.cluster_id.str() + " lacks storage class " +
                        storage_class,
                    ErrorCode::DESTINATION_INELIGIBLE);
    }
  }
  for (const std::string& tag : constraints.required_sovereignty_tags) {
    if (!candidate.capabilities.has_tag(tag)) {
      return reject("required_sovereignty_tag",
                    "cluster " + candidate.cluster_id.str() +
                        " lacks data sovereignty tag " + tag,
                    ErrorCode::DESTINATION_INELIGIBLE);
    }
  }
  if (constraints.require_encryption_at_rest && !candidate.capabilities.encryption_at_rest) {
    return reject("required_encryption",
                  "cluster " + candidate.cluster_id.str() +
                      " cannot attest encryption at rest",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  if (constraints.require_attestation && !candidate.capabilities.attestation) {
    return reject("required_attestation",
                  "cluster " + candidate.cluster_id.str() + " cannot attest its platform",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  if (constraints.require_compatibility_verification) {
    if (candidate.compatibility == CompatibilityResult::INCOMPATIBLE) {
      return reject("compatibility",
                    "cluster " + candidate.cluster_id.str() +
                        " is incompatible with the state compatibility identity",
                    ErrorCode::INCOMPATIBLE);
    }
    if (candidate.compatibility != CompatibilityResult::COMPATIBLE) {
      return reject("compatibility",
                    std::string("cluster ") + candidate.cluster_id.str() +
                        " compatibility evidence is " + to_string(candidate.compatibility),
                    ErrorCode::COMPATIBILITY_UNKNOWN);
    }
  }
  if (constraints.require_integrity_verification && !candidate.integrity_evidence_available) {
    return reject("integrity_evidence",
                  "cluster " + candidate.cluster_id.str() +
                      " has no available integrity evidence for this generation",
                  ErrorCode::INTEGRITY_UNKNOWN);
  }
  if (constraints.minimum_durability != DurabilityClass::UNKNOWN) {
    const bool sufficient =
        candidate.achievable_durability == ReplicaDurabilityState::DURABLE ||
        candidate.achievable_durability == ReplicaDurabilityState::ARCHIVAL ||
        (constraints.minimum_durability <= DurabilityClass::STANDARD &&
         candidate.achievable_durability == ReplicaDurabilityState::STANDARD);
    if (!sufficient) {
      return reject("minimum_durability",
                    std::string("cluster ") + candidate.cluster_id.str() +
                        " cannot establish the required durability class " +
                        to_string(constraints.minimum_durability),
                    ErrorCode::DESTINATION_INELIGIBLE);
    }
  }
  if (constraints.required_free_capacity.known()) {
    if (!candidate.capacity.has_room_for(constraints.required_free_capacity.bytes)) {
      return reject("capacity",
                    "cluster " + candidate.cluster_id.str() + " has " +
                        std::to_string(candidate.capacity.free_logical_bytes()) +
                        " free bytes, below the required " +
                        std::to_string(constraints.required_free_capacity.bytes),
                    ErrorCode::CAPACITY_INSUFFICIENT);
    }
  }
  if (candidate.logical_size.known() && candidate.capacity.total_logical_bytes > 0U) {
    if (!candidate.capacity.has_room_for(candidate.logical_size.bytes)) {
      return reject("capacity",
                    "cluster " + candidate.cluster_id.str() +
                        " cannot hold the state's logical size",
                    ErrorCode::CAPACITY_INSUFFICIENT);
    }
  }
  if (constraints.maximum_evidence_staleness_ticks != 0U) {
    if (!candidate.evidence.present || candidate.evidence.revalidation_required) {
      return reject("evidence_freshness",
                    "cluster " + candidate.cluster_id.str() +
                        " has no current evidence publication",
                    ErrorCode::STALE_EVIDENCE);
    }
    const Tick now = request.now;
    if (candidate.evidence.published_at < now &&
        (now - candidate.evidence.published_at) > constraints.maximum_evidence_staleness_ticks) {
      return reject("evidence_freshness",
                    "cluster " + candidate.cluster_id.str() + " evidence is stale",
                    ErrorCode::STALE_EVIDENCE);
    }
  }
  if (constraints.minimum_cluster_diversity > 0U &&
      static_cast<std::uint32_t>(request.current_clusters.size()) <
          constraints.minimum_cluster_diversity &&
      contains_cluster(request.current_clusters, candidate.cluster_id)) {
    return reject("cluster_diversity",
                  "cluster " + candidate.cluster_id.str() +
                      " does not improve cluster diversity",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  if (constraints.minimum_region_diversity > 0U &&
      static_cast<std::uint32_t>(request.current_regions.size()) <
          constraints.minimum_region_diversity &&
      contains_region(request.current_regions, candidate.region)) {
    return reject("region_diversity",
                  "region " + candidate.region.str() + " does not improve region diversity",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  if (constraints.minimum_failure_domain_diversity > 0U &&
      static_cast<std::uint32_t>(request.current_failure_domains.size()) <
          constraints.minimum_failure_domain_diversity &&
      contains_domain(request.current_failure_domains, candidate.failure_domain)) {
    return reject("failure_domain_diversity",
                  "failure domain " + candidate.failure_domain.str() +
                      " does not improve failure domain diversity",
                  ErrorCode::DESTINATION_INELIGIBLE);
  }
  if (constraints.maximum_transfer_cost.known()) {
    if (!candidate.transfer_cost.known()) {
      return reject("maximum_transfer_cost",
                    "cluster " + candidate.cluster_id.str() +
                        " has no known transfer cost while policy bounds transfer cost",
                    ErrorCode::DESTINATION_INELIGIBLE);
    }
    if (cost_compare(candidate.transfer_cost, constraints.maximum_transfer_cost) > 0) {
      return reject("maximum_transfer_cost",
                    "cluster " + candidate.cluster_id.str() +
                        " transfer cost exceeds the policy bound",
                    ErrorCode::POLICY_VIOLATION);
    }
  }

  outcome.accepted = true;
  outcome.constraint = "none";
  outcome.code = ErrorCode::OK;
  return outcome;
}

RankedCandidate score_candidate(const PlacementCandidate& candidate,
                                const PlacementRequest& request,
                                const StateGenerationRecord* state) {
  RankedCandidate ranked;
  ranked.cluster_id = candidate.cluster_id;
  const RankingWeights& weights = request.policy.weights;

  std::vector<FactorInput> inputs;
  inputs.reserve(16);

  // Fixed factor evaluation order. This order is part of the public model.
  {
    FactorInput input{RankingFactor::EXPECTED_REUSE_VALUE, false, 0, ""};
    if (candidate.expected_reuse.known()) {
      input.normalized = normalize_u64(candidate.expected_reuse.expected_value_milli(),
                                       kReuseCapMilli);
      input.evidence = describe(candidate.expected_reuse);
    } else if (state != nullptr && state->expected_reuse.known()) {
      input.normalized = normalize_u64(state->expected_reuse.expected_value_milli(),
                                       kReuseCapMilli);
      input.evidence = std::string("state ") + describe(state->expected_reuse);
    } else {
      input.unknown = true;
      input.evidence = "UNKNOWN expected reuse";
    }
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::TRANSFER_COST, false, 0, ""};
    if (candidate.transfer_cost.known()) {
      input.normalized = normalize_cost(candidate.transfer_cost);
      input.evidence = describe(candidate.transfer_cost,
                                request.policy.cost_policy.unit_label);
    } else {
      input.unknown = true;
      input.evidence = "UNKNOWN transfer cost";
    }
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::RECONSTRUCTION_COST, false, 0, ""};
    if (candidate.reconstruction_cost.known()) {
      input.normalized = normalize_cost(candidate.reconstruction_cost);
      input.evidence = describe(candidate.reconstruction_cost,
                                request.policy.cost_policy.unit_label);
    } else {
      input.unknown = true;
      input.evidence = "UNKNOWN reconstruction cost";
    }
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::AVAILABLE_BANDWIDTH, false, 0, ""};
    input.normalized = normalize_u64(candidate.capacity.inbound_bytes_per_tick,
                                     kBandwidthCapBytesPerTick);
    input.evidence = std::to_string(candidate.capacity.inbound_bytes_per_tick) +
                     " bytes per tick inbound";
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::PREDICTED_TRANSFER_DURATION, false, 0, ""};
    input.normalized = normalize_u64(candidate.predicted_transfer_ticks, kDurationCapTicks);
    input.evidence = std::to_string(candidate.predicted_transfer_ticks) + " ticks predicted";
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::STORAGE_COST, false, 0, ""};
    if (candidate.storage_cost.known()) {
      input.normalized = normalize_cost(candidate.storage_cost);
      input.evidence = describe(candidate.storage_cost, request.policy.cost_policy.unit_label);
    } else {
      input.unknown = true;
      input.evidence = "UNKNOWN storage cost";
    }
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::COMPUTE_COST, false, 0, ""};
    if (candidate.compute_cost.known()) {
      input.normalized = normalize_cost(candidate.compute_cost);
      input.evidence = describe(candidate.compute_cost, request.policy.cost_policy.unit_label);
    } else {
      input.unknown = true;
      input.evidence = "UNKNOWN compute cost";
    }
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::REGION_AFFINITY, false, 0, ""};
    input.normalized = static_cast<std::int64_t>(std::min<std::uint32_t>(
        candidate.region_affinity, static_cast<std::uint32_t>(kFixedOne)));
    input.evidence = "policy supplied region affinity " +
                     std::to_string(candidate.region_affinity);
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::WORKLOAD_AFFINITY, false, 0, ""};
    input.normalized = static_cast<std::int64_t>(
        std::min<std::uint32_t>(candidate.affinity_score, static_cast<std::uint32_t>(kFixedOne)));
    input.evidence = "policy supplied workload affinity " +
                     std::to_string(candidate.affinity_score);
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::EXISTING_REPLICA_PROXIMITY, false, 0, ""};
    input.normalized = candidate.existing_replica_count > 0U ? kFixedOne : 0;
    input.evidence = std::to_string(candidate.existing_replica_count) +
                     " existing replica(s) of this state in the cluster";
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::FAILURE_DOMAIN_DIVERSITY, false, 0, ""};
    const bool cluster_present = contains_cluster(request.current_clusters, candidate.cluster_id);
    const bool region_present = contains_region(request.current_regions, candidate.region);
    const bool domain_present =
        contains_domain(request.current_failure_domains, candidate.failure_domain);
    const std::int64_t cluster_score = diversity_score(
        cluster_present, static_cast<std::uint32_t>(request.current_clusters.size()),
        request.replication_policy.requirements.cluster_diversity);
    const std::int64_t region_score = diversity_score(
        region_present, static_cast<std::uint32_t>(request.current_regions.size()),
        request.replication_policy.requirements.region_diversity);
    const std::int64_t domain_score = diversity_score(
        domain_present, static_cast<std::uint32_t>(request.current_failure_domains.size()),
        request.replication_policy.requirements.failure_domain_diversity);
    input.normalized = std::min(std::min(cluster_score, region_score), domain_score);
    input.evidence = std::string("cluster ") + (cluster_present ? "present" : "new") +
                     ", region " + (region_present ? "present" : "new") + ", failure domain " +
                     (domain_present ? "present" : "new");
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::EXPECTED_FUTURE_DEMAND, false, 0, ""};
    input.normalized = static_cast<std::int64_t>(std::min<std::uint32_t>(
        candidate.expected_future_demand, static_cast<std::uint32_t>(kFixedOne)));
    input.evidence = "policy supplied future demand " +
                     std::to_string(candidate.expected_future_demand);
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::SOURCE_HEALTH, false, 0, ""};
    input.normalized = candidate.source_cluster_healthy ? kFixedOne : 0;
    input.evidence = candidate.source_cluster_healthy ? "source cluster healthy"
                                                      : "source cluster not healthy";
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::DESTINATION_HEALTH, false, 0, ""};
    input.normalized = health_score(candidate.health);
    input.evidence = std::string("destination health ") + to_string(candidate.health);
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::PRESSURE, false, 0, ""};
    const std::int64_t utilization = static_cast<std::int64_t>(
        std::min<std::uint32_t>(candidate.capacity.utilization_per_mille, 1000U));
    input.normalized = kFixedOne - (utilization * kFixedOne) / 1000;
    input.evidence = "cluster utilization " + std::to_string(utilization) + " per mille";
    inputs.push_back(std::move(input));
  }
  {
    FactorInput input{RankingFactor::UTILIZATION, false, 0, ""};
    const std::int64_t in_flight = static_cast<std::int64_t>(
        std::min<std::uint32_t>(candidate.in_flight_operations, 1000U));
    input.normalized = kFixedOne - (in_flight * kFixedOne) / 1000;
    input.evidence = std::to_string(candidate.in_flight_operations) +
                     " in-flight operation(s) in the cluster";
    inputs.push_back(std::move(input));
  }

  for (const FactorInput& input : inputs) {
    RankingContribution contribution;
    contribution.factor = input.factor;
    contribution.weight = weights.weight_of(input.factor);
    contribution.unknown = input.unknown;
    contribution.evidence = input.evidence;
    if (input.unknown) {
      // Pessimistic treatment of absent evidence: score the factor as the worst
      // value the policy's weight sign could prefer.
      contribution.normalized_value = contribution.weight >= 0 ? 0 : kFixedOne;
    } else {
      contribution.normalized_value = input.normalized;
    }
    contribution.contribution =
        saturating_mul(contribution.weight, contribution.normalized_value);
    ranked.score = saturating_add(ranked.score, contribution.contribution);
    ranked.contributions.push_back(std::move(contribution));
  }

  return ranked;
}

PlacementEvaluation evaluate_placement(const PlacementRequest& request,
                                       const StateGenerationRecord* state) {
  PlacementEvaluation evaluation;
  evaluation.state_id = request.state_id;
  evaluation.state_generation = request.state_generation;
  evaluation.placement_policy_id = request.policy.placement_policy_id;
  evaluation.placement_policy_generation = request.policy.generation;

  std::vector<RankedCandidate> accepted;
  for (const PlacementCandidate& candidate : request.candidates) {
    ConstraintOutcome outcome = evaluate_hard_constraints(candidate, request, state);
    if (!outcome.accepted) {
      PlacementRejection rejection;
      rejection.cluster_id = candidate.cluster_id;
      rejection.constraint = outcome.constraint;
      rejection.detail = outcome.detail;
      rejection.code = outcome.code;
      evaluation.rejected.push_back(std::move(rejection));
      continue;
    }
    RankedCandidate ranked = score_candidate(candidate, request, state);
    ranked.cluster_id = candidate.cluster_id;
    accepted.push_back(std::move(ranked));
  }

  std::sort(evaluation.rejected.begin(), evaluation.rejected.end(),
            [](const PlacementRejection& a, const PlacementRejection& b) {
              if (a.cluster_id != b.cluster_id) {
                return a.cluster_id < b.cluster_id;
              }
              if (a.constraint != b.constraint) {
                return a.constraint < b.constraint;
              }
              return a.detail < b.detail;
            });

  std::sort(accepted.begin(), accepted.end(), [](const RankedCandidate& a,
                                                 const RankedCandidate& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    const auto diversity = [](const RankedCandidate& candidate) {
      for (const RankingContribution& contribution : candidate.contributions) {
        if (contribution.factor == RankingFactor::FAILURE_DOMAIN_DIVERSITY) {
          return contribution.normalized_value;
        }
      }
      return static_cast<std::int64_t>(0);
    };
    const auto affinity = [](const RankedCandidate& candidate) {
      for (const RankingContribution& contribution : candidate.contributions) {
        if (contribution.factor == RankingFactor::REGION_AFFINITY) {
          return contribution.normalized_value;
        }
      }
      return static_cast<std::int64_t>(0);
    };
    if (diversity(a) != diversity(b)) {
      return diversity(a) > diversity(b);
    }
    if (affinity(a) != affinity(b)) {
      return affinity(a) > affinity(b);
    }
    return a.cluster_id < b.cluster_id;
  });

  evaluation.ranked = std::move(accepted);

  if (evaluation.ranked.empty()) {
    evaluation.decision = DecisionKind::REJECT;
    evaluation.reasons.push_back("no candidate satisfied every hard constraint");
    for (const PlacementRejection& rejection : evaluation.rejected) {
      evaluation.reasons.push_back("cluster " + rejection.cluster_id.str() + " rejected by " +
                                   rejection.constraint + ": " + rejection.detail);
    }
    return evaluation;
  }

  evaluation.chosen_cluster = evaluation.ranked.front().cluster_id;
  evaluation.chosen_score = evaluation.ranked.front().score;
  evaluation.decision = request.intent;
  evaluation.reasons.push_back("cluster " + evaluation.chosen_cluster.str() +
                               " selected with deterministic score " +
                               std::to_string(evaluation.chosen_score));
  for (const RankingContribution& contribution : evaluation.ranked.front().contributions) {
    if (contribution.weight == 0) {
      continue;
    }
    std::string line = "factor ";
    line += to_string(contribution.factor);
    line += ": weight " + std::to_string(contribution.weight) + ", value " +
            std::to_string(contribution.normalized_value) + ", contribution " +
            std::to_string(contribution.contribution);
    if (contribution.unknown) {
      line += " (evidence absent; scored pessimistically)";
    } else if (!contribution.evidence.empty()) {
      line += " (";
      line += contribution.evidence;
      line += ")";
    }
    evaluation.reasons.push_back(std::move(line));
  }
  if (!evaluation.rejected.empty()) {
    evaluation.reasons.push_back(std::to_string(evaluation.rejected.size()) +
                                 " candidate(s) rejected by hard constraints");
  }
  return evaluation;
}

}  // namespace ccsf
