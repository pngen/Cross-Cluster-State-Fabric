// Cross-Cluster State Fabric - deterministic placement evaluation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CCSF_PLACEMENT_HPP
#define CCSF_PLACEMENT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ccsf/cluster.hpp"
#include "ccsf/cost.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/policy.hpp"
#include "ccsf/state.hpp"
#include "ccsf/taxonomy.hpp"

namespace ccsf {

/// Evidence about one candidate destination. Nothing here is authority; it is
/// the input the deterministic policy engine ranks.
struct PlacementCandidate {
  ClusterId cluster_id;
  ClusterIncarnationId cluster_incarnation;
  RegionId region;
  FailureDomainId failure_domain;
  ClusterHealthState health{ClusterHealthState::UNKNOWN};
  ClusterCapabilities capabilities;
  ClusterCapacity capacity;
  ClusterEvidence evidence;

  bool already_hosts_current_replica{false};
  CompatibilityResult compatibility{CompatibilityResult::UNKNOWN};
  bool integrity_evidence_available{false};
  SizeValue logical_size;
  SizeValue transfer_bytes;
  CostValue transfer_cost;
  CostValue reconstruction_cost;
  CostValue storage_cost;
  CostValue compute_cost;
  ReuseValue expected_reuse;
  std::uint64_t predicted_transfer_ticks{0};
  std::uint64_t predicted_wait_ticks{0};
  std::uint32_t existing_replica_count{0};
  std::uint32_t in_flight_operations{0};
  std::uint32_t affinity_score{0};
  std::uint32_t region_affinity{0};
  std::uint32_t expected_future_demand{0};
  bool source_cluster_healthy{false};
  ReplicaDurabilityState achievable_durability{ReplicaDurabilityState::UNKNOWN};
};

/// A single placement decision request.
struct PlacementRequest {
  StateId state_id;
  StateGeneration state_generation;
  PlacementPolicyRecord policy;
  ReplicationPolicyRecord replication_policy;
  std::vector<PlacementCandidate> candidates;
  ClusterId preferred_source;
  bool destination_is_new_replica{true};

  /// Decision kind reported when a candidate is accepted.
  DecisionKind intent{DecisionKind::REPLICATE};

  /// Coordinator logical time used for evidence freshness checks.
  Tick now{0};

  /// Diversity already present in the current replica set. A candidate can only
  /// satisfy a diversity floor it actually improves.
  std::vector<ClusterId> current_clusters;
  std::vector<RegionId> current_regions;
  std::vector<FailureDomainId> current_failure_domains;
};

/// A candidate rejected by a hard constraint.
struct PlacementRejection {
  ClusterId cluster_id;
  std::string constraint;
  std::string detail;
  ErrorCode code{ErrorCode::INVALID_ARGUMENT};
};

/// One ranked candidate with its full factor breakdown.
struct RankedCandidate {
  ClusterId cluster_id;
  std::int64_t score{0};
  std::vector<RankingContribution> contributions;
};

/// The deterministic output of evaluate_placement.
struct PlacementEvaluation {
  StateId state_id;
  StateGeneration state_generation;
  PlacementPolicyId placement_policy_id;
  PlacementPolicyGeneration placement_policy_generation;
  DecisionKind decision{DecisionKind::UNKNOWN};
  ClusterId chosen_cluster;
  std::int64_t chosen_score{0};
  std::vector<PlacementRejection> rejected;
  std::vector<RankedCandidate> ranked;
  std::vector<std::string> reasons;
};

/// Evaluates hard constraints for one candidate. Returns OK when the candidate
/// satisfies every constraint, otherwise the specific rejection.
struct ConstraintOutcome {
  bool accepted{false};
  std::string constraint;
  std::string detail;
  ErrorCode code{ErrorCode::OK};
};

ConstraintOutcome evaluate_hard_constraints(const PlacementCandidate& candidate,
                                            const PlacementRequest& request,
                                            const StateGenerationRecord* state);

/// Full deterministic evaluation: hard constraints first, then ranking.
PlacementEvaluation evaluate_placement(const PlacementRequest& request,
                                       const StateGenerationRecord* state);

/// Deterministic score of one candidate (used by the planner and by tests).
RankedCandidate score_candidate(const PlacementCandidate& candidate,
                                const PlacementRequest& request,
                                const StateGenerationRecord* state);

}  // namespace ccsf

#endif  // CCSF_PLACEMENT_HPP
