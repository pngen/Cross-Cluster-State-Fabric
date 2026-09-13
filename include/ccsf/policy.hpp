// Cross-Cluster State Fabric - placement and replication policy.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Policy is split into hard constraints and ranking factors. Hard constraints
// are evaluated first and are absolute: no score can rescue a candidate that
// failed a hard constraint. Ranking is fully deterministic integer arithmetic
// over named factors with stable tie-breaking.

#ifndef CCSF_POLICY_HPP
#define CCSF_POLICY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ccsf/clock.hpp"
#include "ccsf/cost.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/taxonomy.hpp"

namespace ccsf {

inline constexpr std::size_t kMaxPolicyListEntries = 4096;
inline constexpr std::size_t kMaxPolicyStringLength = 256;
inline constexpr std::size_t kMaxRankingFactors = 16;

/// Hard placement constraints. An empty collection means "unconstrained".
struct PlacementConstraints {
  std::vector<ClusterId> legal_clusters;
  std::vector<RegionId> legal_regions;
  std::vector<ClusterId> prohibited_clusters;
  std::vector<RegionId> prohibited_regions;
  std::vector<std::string> required_capabilities;
  std::vector<std::string> required_storage_classes;
  std::vector<std::string> required_sovereignty_tags;

  /// When set, a candidate must reside in one of the listed clusters.
  bool require_locality{false};
  std::vector<ClusterId> locality_clusters;

  /// Diversity required of the replica set after the candidate joins.
  std::uint32_t minimum_cluster_diversity{0};
  std::uint32_t minimum_region_diversity{0};
  std::uint32_t minimum_failure_domain_diversity{0};

  /// Minimum durability the destination must be able to establish.
  DurabilityClass minimum_durability{DurabilityClass::UNKNOWN};

  /// Required cluster health floor. UNKNOWN means "any health".
  ClusterHealthState required_health{ClusterHealthState::UNKNOWN};

  /// Evidence freshness horizon in coordinator ticks. 0 disables the check.
  std::uint64_t maximum_evidence_staleness_ticks{0};

  /// Required free destination capacity in bytes. Unknown disables the check.
  SizeValue required_free_capacity{};

  /// Upper bound on the modeled transfer cost. Unknown disables the check.
  CostValue maximum_transfer_cost{};

  bool require_encryption_at_rest{false};
  bool require_attestation{false};
  bool require_integrity_verification{true};
  bool require_compatibility_verification{true};

  /// Sorts and de-duplicates every list so that equal policy content has one
  /// canonical representation and therefore one fingerprint.
  void canonicalize();
};

/// Named ranking factors, in the fixed evaluation order used by the planner.
enum class RankingFactor : std::uint16_t {
  EXPECTED_REUSE_VALUE = 0,
  TRANSFER_COST = 1,
  RECONSTRUCTION_COST = 2,
  AVAILABLE_BANDWIDTH = 3,
  PREDICTED_TRANSFER_DURATION = 4,
  STORAGE_COST = 5,
  COMPUTE_COST = 6,
  REGION_AFFINITY = 7,
  WORKLOAD_AFFINITY = 8,
  EXISTING_REPLICA_PROXIMITY = 9,
  FAILURE_DOMAIN_DIVERSITY = 10,
  EXPECTED_FUTURE_DEMAND = 11,
  SOURCE_HEALTH = 12,
  DESTINATION_HEALTH = 13,
  PRESSURE = 14,
  UTILIZATION = 15
};

const char* to_string(RankingFactor factor) noexcept;
bool is_valid(RankingFactor factor) noexcept;

/// Weights for the named ranking factors. A zero weight disables a factor from
/// contributing to the score, but the factor is still reported in explanations
/// when it was evaluated.
struct RankingWeights {
  std::int64_t expected_reuse_value{0};
  std::int64_t transfer_cost{0};
  std::int64_t reconstruction_cost{0};
  std::int64_t available_bandwidth{0};
  std::int64_t predicted_transfer_duration{0};
  std::int64_t storage_cost{0};
  std::int64_t compute_cost{0};
  std::int64_t region_affinity{0};
  std::int64_t workload_affinity{0};
  std::int64_t existing_replica_proximity{0};
  std::int64_t failure_domain_diversity{0};
  std::int64_t expected_future_demand{0};
  std::int64_t source_health{0};
  std::int64_t destination_health{0};
  std::int64_t pressure{0};
  std::int64_t utilization{0};

  [[nodiscard]] std::int64_t weight_of(RankingFactor factor) const noexcept;
  void set_weight(RankingFactor factor, std::int64_t weight) noexcept;
};

/// Policy-supplied unit prices. Every entry is optional and carries its own
/// provenance; an absent price yields UNKNOWN modeled cost, never zero cost.
struct CostPolicy {
  CostSource source{CostSource::UNKNOWN};
  std::int64_t transfer_micro_per_mib{0};
  std::int64_t egress_micro_per_mib{0};
  std::int64_t storage_micro_per_mib_tick{0};
  std::int64_t compute_micro_per_mib{0};
  std::int64_t accelerator_micro_per_mib_tick{0};
  std::int64_t repair_micro_per_mib{0};
  std::string unit_label{"units"};

  [[nodiscard]] bool priced() const noexcept { return source != CostSource::UNKNOWN; }

  /// Applies a policy price to a byte count, yielding a provenance-carrying cost.
  CostValue price_bytes(std::int64_t micro_per_mib, SizeValue bytes) const noexcept;
};

/// One named contribution to a candidate's ranking score.
struct RankingContribution {
  RankingFactor factor{RankingFactor::EXPECTED_REUSE_VALUE};
  std::int64_t weight{0};
  /// Factor value normalized to the fixed-point range [0, 1000000].
  std::int64_t normalized_value{0};
  /// Set when the underlying evidence was absent for this candidate.
  bool unknown{false};
  std::int64_t contribution{0};
  std::string evidence;
};

/// Deterministic placement policy.
struct PlacementPolicyRecord {
  PlacementPolicyId placement_policy_id;
  PlacementPolicyGeneration generation;
  PlacementConstraints constraints;
  RankingWeights weights;
  CostPolicy cost_policy;
  Tick updated_at{0};

  [[nodiscard]] std::string fingerprint() const;
};

/// Replication requirements attached to a state generation.
struct ReplicationRequirements {
  ReplicationPolicyForm form{ReplicationPolicyForm::N_REPLICAS};
  std::uint32_t minimum_authoritative_replicas{1};
  std::uint32_t desired_replicas{1};
  std::uint32_t maximum_replicas{4};
  std::uint32_t region_diversity{0};
  std::uint32_t cluster_diversity{0};
  std::uint32_t failure_domain_diversity{0};
  DurabilityClass minimum_durability{DurabilityClass::UNKNOWN};

  friend bool operator==(const ReplicationRequirements&,
                         const ReplicationRequirements&) = default;
};

/// Eligibility rules applied to replication/migration endpoints.
struct EligibilityRules {
  std::vector<ClusterId> allowed_clusters;
  std::vector<ClusterId> denied_clusters;
  bool require_healthy_source{true};
  bool require_healthy_destination{true};
  bool require_available_capacity{true};
  bool allow_same_cluster{false};
  bool require_fresh_evidence{true};
  std::uint32_t maximum_in_flight_operations_per_state{2};

  void canonicalize();
};

/// Authority rules applied when deciding whether a replica may serve reuse.
struct ReuseAuthorityRules {
  bool require_verified_integrity{true};
  bool require_verified_compatibility{true};
  bool require_current_state_generation{true};
  bool require_replica_set_membership{true};
  bool require_current_cluster_incarnation{true};
  bool require_current_coordinator_epoch{true};
  bool allow_historical_read_only{false};
};

/// Retention policy for stale copies and historical records.
struct RetentionPolicy {
  bool retain_historical_records{true};
  std::uint64_t stale_copy_retirement_ticks{0};
  std::uint32_t maximum_replicas_per_state{8};
  std::uint32_t hot_history_records_per_state{64};
};

/// Repair policy applied when a replica set is under-replicated.
struct RepairPolicy {
  bool automatic_repair_enabled{true};
  std::uint32_t maximum_concurrent_repairs{1};
  std::uint32_t repair_attempt_limit{3};
};

/// Evidence generations that must be at least current for reuse authority.
struct RevalidationRequirements {
  bool require_revalidation_after_cluster_reincarnation{true};
  bool require_revalidation_after_coordinator_restart{true};
  bool require_fresh_health_evidence{true};
  bool require_fresh_capability_evidence{true};
};

/// Deterministic replication policy.
struct ReplicationPolicyRecord {
  ReplicationPolicyId replication_policy_id;
  ReplicationPolicyGeneration generation;
  ReplicationRequirements requirements;
  EligibilityRules source_rules;
  EligibilityRules destination_rules;
  ReuseAuthorityRules reuse_rules;
  RetentionPolicy retention;
  RepairPolicy repair;
  RevalidationRequirements revalidation;
  DegradedBehavior degraded_behavior{DegradedBehavior::REJECT_REUSE};
  OperationFailureAction failure_action{OperationFailureAction::ABANDON_DESTINATION};
  Tick updated_at{0};

  /// Derives the effective diversity floors implied by the policy form.
  [[nodiscard]] ReplicationRequirements effective_requirements() const;

  [[nodiscard]] std::string fingerprint() const;
};

/// Builds a default replication policy for a given form.
ReplicationPolicyRecord make_replication_policy(ReplicationPolicyId id,
                                                ReplicationPolicyGeneration generation,
                                                ReplicationPolicyForm form,
                                                std::uint32_t replica_count);

}  // namespace ccsf

#endif  // CCSF_POLICY_HPP
